// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <stdint.h>

#include "runtime/snapshot_loader.h"

#include "gen_cpp/PaloBrokerService_types.h"
#include "gen_cpp/TPaloBrokerService.h"

#include "common/logging.h"
#include "exec/broker_reader.h"
#include "exec/broker_writer.h"
#include "olap/file_helper.h"
#include "olap/olap_engine.h"
#include "olap/olap_table.h"
#include "runtime/exec_env.h"
#include "runtime/broker_mgr.h"
#include "util/file_utils.h"

namespace doris {

#ifdef BE_TEST
inline BrokerServiceClientCache* client_cache(ExecEnv* env) {
    static BrokerServiceClientCache s_client_cache;
    return &s_client_cache;
}

inline const std::string& client_id(ExecEnv* env, const TNetworkAddress& addr) {
    static std::string s_client_id = "doris_unit_test";
    return s_client_id;
}
#else
inline BrokerServiceClientCache* client_cache(ExecEnv* env) {
    return env->broker_client_cache();
}

inline const std::string& client_id(ExecEnv* env, const TNetworkAddress& addr) {
    return env->broker_mgr()->get_client_id(addr);
}
#endif

SnapshotLoader::SnapshotLoader(ExecEnv* env) :
    _env(env) {

}

SnapshotLoader::~SnapshotLoader() {

}

Status SnapshotLoader::upload(
        const std::map<std::string, std::string>& src_to_dest_path,
        const TNetworkAddress& broker_addr,
        const std::map<std::string, std::string>& broker_prop,
        int64_t job_id,
        std::map<int64_t, std::vector<std::string>>* tablet_files) {

    LOG(INFO) << "begin to upload snapshot files. num: "
              << src_to_dest_path.size() << ", broker addr: "
              << broker_addr << ", job: " << job_id;

    Status status = Status::OK;
    // 1. validate local tablet snapshot paths
    RETURN_IF_ERROR(_check_local_snapshot_paths(src_to_dest_path, true));

    // 2. get broker client
    BrokerServiceConnection client(client_cache(_env), broker_addr, 10000, &status);
    if (!status.ok()) {
        std::stringstream ss;
        ss << "failed to get broker client. "
            << "broker addr: " << broker_addr
            << ". msg: " << status.get_error_msg();
        LOG(WARNING) << ss.str();
        return Status(ss.str());
    }

    std::vector<TNetworkAddress> broker_addrs;
    broker_addrs.push_back(broker_addr);
    // 3. for each src path, upload it to remote storage
    for (auto iter = src_to_dest_path.begin(); iter != src_to_dest_path.end();
            iter++) {
        const std::string& src_path = iter->first;
        const std::string& dest_path = iter->second;

        int64_t tablet_id = 0;
        int32_t schema_hash = 0;
        RETURN_IF_ERROR(_get_tablet_id_and_schema_hash_from_file_path(
                src_path, &tablet_id, &schema_hash));

        // 2.1 get existing files from remote path
        std::map<std::string, FileStat> remote_files;
        RETURN_IF_ERROR(_get_existing_files_from_remote(
                client, dest_path, broker_prop, &remote_files));

        for (auto& tmp : remote_files) {
            VLOG(2) << "get remote file: " << tmp.first << ", checksum: " << tmp.second.md5;
        }

        // 2.2 list local files
        std::vector<std::string> local_files;
        std::vector<std::string> local_files_with_checksum;
        RETURN_IF_ERROR(_get_existing_files_from_local(src_path, &local_files));

        // 2.3 iterate local files
        for (auto it = local_files.begin(); it != local_files.end(); it++) {
            const std::string& local_file = *it;
            // calc md5sum of localfile
            std::string md5sum;
            status = FileUtils::md5sum(src_path + "/" + local_file, &md5sum);
            if (!status.ok()) {
                std::stringstream ss;
                ss << "failed to get md5sum of file: " << local_file 
                    << ": " << status.get_error_msg();
                LOG(WARNING) << ss.str();
                return Status(ss.str());
            }
            VLOG(2) << "get file checksum: " << local_file << ": " << md5sum;
            local_files_with_checksum.push_back(local_file + "." + md5sum);

            // check if this local file need upload
            bool need_upload = false;
            auto find = remote_files.find(local_file);
            if (find != remote_files.end()) {
                if (md5sum != find->second.md5) {
                    // remote storage file exist, but with different checksum
                    LOG(WARNING) << "remote file checksum is invalid. remote: " << find->first
                                 << ", local: " << md5sum;
                    // TODO(cmy): save these files and delete them later
                    need_upload = true;
                }
            } else {
                need_upload = true;
            }

            if (!need_upload) {
                VLOG(2) << "file exist in remote path, no need to upload: " << local_file;
                continue;
            }

            // upload
            // open broker writer. file name end with ".part"
            // it will be rename to ".md5sum" after upload finished
            std::string full_remote_file = dest_path + "/" + local_file;
            {
                // NOTICE: broker writer must be closed before calling rename
                std::unique_ptr<BrokerWriter> broker_writer;
                broker_writer.reset(new BrokerWriter(_env,
                    broker_addrs,
                    broker_prop,
                    full_remote_file + ".part",
                    0 /* offset */));
                RETURN_IF_ERROR(broker_writer->open());

                // read file and write to broker
                std::string full_local_file = src_path + "/" + local_file;
                FileHandler file_handler;
                OLAPStatus ost = file_handler.open(full_local_file, O_RDONLY);
                if (ost != OLAP_SUCCESS) {
                    return Status("failed to open file: " + full_local_file);
                }

                size_t file_len = file_handler.length();
                if (file_len == -1) {
                    return Status("failed to get length of file: " + full_local_file);
                }

                constexpr size_t buf_sz = 1024 * 1024;
                char read_buf[buf_sz];
                size_t left_len = file_len;
                size_t read_offset = 0;
                while (left_len > 0) {
                    size_t read_len = left_len > buf_sz ? buf_sz : left_len;
                    ost = file_handler.pread(read_buf, read_len, read_offset);
                    if (ost != OLAP_SUCCESS) {
                        return Status("failed to read file: " + full_local_file);
                    }
                    // write through broker
                    size_t write_len = 0;
                    RETURN_IF_ERROR(broker_writer->write(reinterpret_cast<const uint8_t*>(read_buf),
                        read_len, &write_len));
                    DCHECK_EQ(write_len, read_len);

                    read_offset += read_len;
                    left_len -= read_len;
                }
                LOG(INFO) << "finished to write file via broker. file: " <<
                    full_local_file << ", length: " << file_len;
            }

            // rename file to end with ".md5sum"
            RETURN_IF_ERROR(_rename_remote_file(client,
                    full_remote_file + ".part",
                    full_remote_file + "." + md5sum,
                    broker_prop));
        } // end for each tablet's local files

        tablet_files->emplace(tablet_id, local_files_with_checksum);
        LOG(INFO) << "finished to write tablet to remote. local path: "
                << src_path << ", remote path: " << dest_path;
    } // end for each tablet path

    LOG(INFO) << "finished to upload snapshots. job: " << job_id;
    return status;
}

/*
 * Download snapshot files from remote.
 * After downloaded, the local dir should contains all files existing in remote,
 * may also contains severval useless files.
 */
Status SnapshotLoader::download(
        const std::map<std::string, std::string>& src_to_dest_path,
        const TNetworkAddress& broker_addr,
        const std::map<std::string, std::string>& broker_prop,
        int64_t job_id,
        std::vector<int64_t>* downloaded_tablet_ids) {

    LOG(INFO) << "begin to download snapshot files. num: "
              << src_to_dest_path.size() << ", broker addr: "
              << broker_addr << ", job: " << job_id;

    Status status = Status::OK;
    // 1. validate local tablet snapshot paths
    RETURN_IF_ERROR(_check_local_snapshot_paths(src_to_dest_path, false));

    // 2. get broker client
    BrokerServiceConnection client(client_cache(_env), broker_addr, 10000, &status);
    if (!status.ok()) {
        std::stringstream ss;
        ss << "failed to get broker client. "
            << "broker addr: " << broker_addr
            << ". msg: " << status.get_error_msg();
        LOG(WARNING) << ss.str();
        return Status(ss.str());
    }

    std::vector<TNetworkAddress> broker_addrs;
    broker_addrs.push_back(broker_addr);
    // 3. for each src path, download it to local storage
    for (auto iter = src_to_dest_path.begin(); iter != src_to_dest_path.end();
            iter++) {
        const std::string& remote_path = iter->first;
        const std::string& local_path = iter->second;

        int64_t local_tablet_id = 0;
        int32_t schema_hash = 0;
        RETURN_IF_ERROR(_get_tablet_id_and_schema_hash_from_file_path(
                local_path, &local_tablet_id, &schema_hash));
        downloaded_tablet_ids->push_back(local_tablet_id);

        int64_t remote_tablet_id;
        RETURN_IF_ERROR(_get_tablet_id_from_remote_path(remote_path,
                &remote_tablet_id));
        VLOG(2) << "get local tablet id: " << local_tablet_id << ", schema hash: "
                << schema_hash << ", remote tablet id: " << remote_tablet_id;

        // 1. get local files
        std::vector<std::string> local_files;
        RETURN_IF_ERROR(_get_existing_files_from_local(local_path, &local_files));

        // 2. get remote files
        std::map<std::string, FileStat> remote_files;
        RETURN_IF_ERROR(_get_existing_files_from_remote(
                client, remote_path, broker_prop, &remote_files));
        if (remote_files.empty()) {
            std::stringstream ss;
            ss << "get nothing from remote path: " << remote_path;
            LOG(WARNING) << ss.str();
            return Status(ss.str());
        }

        for (auto& iter : remote_files) {
            bool need_download = false;
            const std::string& remote_file = iter.first;
            const FileStat& file_stat = iter.second;
            auto find = std::find(local_files.begin(), local_files.end(), remote_file);
            if (find == local_files.end()) {
                // remote file does not exist in local, download it
                need_download = true;
            } else {
                if (_end_with(remote_file, ".hdr")) {
                    // this is a header file, download it.
                    need_download = true;
                } else {
                    // check checksum
                    std::string local_md5sum;
                    Status st = FileUtils::md5sum(local_path + "/" + remote_file, &local_md5sum);
                    if (!st.ok()) {
                        LOG(WARNING) << "failed to get md5sum of local file: " << remote_file
                                << ". msg: " << st.get_error_msg() << ". download it";
                        need_download = true;
                    } else {
                        VLOG(2) << "get local file checksum: " << remote_file << ": " << local_md5sum;
                        if (file_stat.md5 != local_md5sum) {
                            // file's checksum does not equal, download it.
                            need_download = true;
                        }
                    }
                }
            }

            if (!need_download) {
                LOG(INFO) << "remote file already exist in local, no need to download."
                          << ", file: " << remote_file;
                continue;
            }

            // begin to download
            std::string full_remote_file = remote_path + "/" + remote_file + "." + file_stat.md5;
            std::string local_file_name;
            // we need to replace the tablet_id in remote file name with local tablet id
            RETURN_IF_ERROR(_replace_tablet_id(remote_file, local_tablet_id, &local_file_name));
            std::string full_local_file = local_path + "/" + local_file_name;
            LOG(INFO) << "begin to download from " << full_remote_file << " to "
                    << full_local_file;
            size_t file_len = file_stat.size;
            {
                // 1. open remote file for read
                std::unique_ptr<BrokerReader> broker_reader;
                broker_reader.reset(new BrokerReader(_env,
                    broker_addrs,
                    broker_prop,
                    full_remote_file,
                    0 /* offset */));
                RETURN_IF_ERROR(broker_reader->open());

                // 2. remove the existing local file if exist
                if (boost::filesystem::remove(full_local_file)) {
                    VLOG(2) << "remove the previously exist local file: "
                            << full_local_file;
                }
                // remove file which will be downloaded now.
                // this file will be added to local_files if it be downloaded successfully.
                local_files.erase(find);

                // 3. open local file for write
                FileHandler file_handler;
                OLAPStatus ost = file_handler.open_with_mode(full_local_file,
                        O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
                if (ost != OLAP_SUCCESS) {
                    return Status("failed to open file: " + full_local_file);
                }

                // 4. read remote and write to local
                VLOG(2) << "read remote file: " << full_remote_file << " to local: "
                        << full_local_file << ". file size: " << file_len;
                constexpr size_t buf_sz = 1024 * 1024;
                char read_buf[buf_sz];
                size_t write_offset = 0;
                bool eof = false;
                while (!eof) {
                    size_t read_len = buf_sz;
                    RETURN_IF_ERROR(broker_reader->read(reinterpret_cast<uint8_t*>(read_buf),
                        &read_len, &eof));

                    if (eof) {
                        continue;
                    }
                    
                    if (read_len > 0) {
                        ost = file_handler.pwrite(read_buf, read_len, write_offset);
                        if (ost != OLAP_SUCCESS) {
                            return Status("failed to write file: " + full_local_file);
                        }

                        write_offset += read_len;
                    }
                }
            } // file_handler should be closed before calculating checksum

            // 5. check md5 of the downloaded file
            std::string downloaded_md5sum;
            status = FileUtils::md5sum(full_local_file, &downloaded_md5sum);
            if (!status.ok()) {
                std::stringstream ss;
                ss << "failed to get md5sum of file: " << full_local_file;
                LOG(WARNING) << ss.str();
                return Status(ss.str());
            }
            VLOG(2) << "get downloaded file checksum: " << full_local_file << ": "
                    << downloaded_md5sum;
            if (downloaded_md5sum != file_stat.md5) {
                std::stringstream ss;
                ss << "invalid md5 of downloaded file: " << full_local_file
                   << ", expected: " << file_stat.md5 << ", get: " << downloaded_md5sum;
                LOG(WARNING) << ss.str();
                return Status(ss.str());
            }

            // local_files always keep the updated local files
            local_files.push_back(local_file_name);
            LOG(INFO) << "finished to download file via broker. file: " <<
                full_local_file << ", length: " << file_len;
        } // end for all remote files

        // finally, delete local files which are not in remote
        for (const auto& local_file: local_files) {
            // replace the tablet id in local file name with the remote tablet id,
            // in order to compare the file name.
            std::string new_name;
            Status st = _replace_tablet_id(local_file, remote_tablet_id, &new_name);
            if (!st.ok()) {
                LOG(WARNING) << "failed to replace tablet id. unknown local file: " << st.get_error_msg()
                        << ". ignore it";
                continue; 
            }
            VLOG(2) << "new file name after replace tablet id: " << new_name;
            const auto& find = remote_files.find(new_name);
            if (find != remote_files.end()) {
                continue;
            }

            // delete
            std::string full_local_file = local_path + "/" + local_file;
            VLOG(2) << "begin to delete local snapshot file: " << full_local_file
                << ", it does not exist in remote";
            if (remove(full_local_file.c_str()) != 0) {
                LOG(WARNING) << "failed to delete unknown local file: " << full_local_file
                        << ", ignore it";
            }
        }
    } // end for src_to_dest_path

    LOG(INFO) << "finished to download snapshots. job: " << job_id;
    return status;
}

// move the snapshot files in snapshot_path
// to tablet_path
// If overwrite, just replace the tablet_path with snapshot_path,
// else: (TODO)
// 
Status SnapshotLoader::move(
    const std::string& snapshot_path,
    const std::string& tablet_path,
    const std::string& store_path,
    int64_t job_id,
    bool overwrite) {

    LOG(INFO) << "begin to move snapshot files. from: "
              << snapshot_path << ", to: " << tablet_path
              << ", store: " << store_path << ", job: " << job_id;

    Status status = Status::OK;

    // validate snapshot_path and tablet_path
    int64_t snapshot_tablet_id = 0;
    int32_t snapshot_schema_hash = 0;
    RETURN_IF_ERROR(_get_tablet_id_and_schema_hash_from_file_path(
            snapshot_path, &snapshot_tablet_id, &snapshot_schema_hash));

    int64_t tablet_id = 0;
    int32_t schema_hash = 0;
    RETURN_IF_ERROR(_get_tablet_id_and_schema_hash_from_file_path(
            tablet_path, &tablet_id, &schema_hash));

    if (tablet_id != snapshot_tablet_id ||
            schema_hash != snapshot_schema_hash) {
        std::stringstream ss;
        ss << "path does not match. snapshot: " << snapshot_path
            << ", tablet path: " << tablet_path;
        LOG(WARNING) << ss.str();
        return Status(ss.str());
    }

    boost::filesystem::path tablet_dir(tablet_path);
    boost::filesystem::path snapshot_dir(snapshot_path);
    if (!boost::filesystem::exists(tablet_dir)) {
        std::stringstream ss;
        ss << "tablet path does not exist: " << tablet_path;
        LOG(WARNING) << ss.str();
        return Status(ss.str());
    }

    if (!boost::filesystem::exists(snapshot_dir)) {
        std::stringstream ss;
        ss << "snapshot path does not exist: " << snapshot_path;
        LOG(WARNING) << ss.str();
        return Status(ss.str());
    }

    if (overwrite) {
        std::vector<std::string> snapshot_files;
        RETURN_IF_ERROR(_get_existing_files_from_local(snapshot_path, &snapshot_files));

        // 1. simply delete the old dir and replace it with the snapshot dir
        try {
            // This remove seems saft enough, because we already get
            // tablet id and schema hash from this path, which
            // means this path is a valid path.
            boost::filesystem::remove_all(tablet_dir);
            VLOG(2) << "remove dir: " << tablet_dir;
            boost::filesystem::create_directory(tablet_dir);
            VLOG(2) << "re-create dir: " << tablet_dir;
        } catch (const boost::filesystem::filesystem_error& e) {
            std::stringstream ss;
            ss << "failed to move tablet path: " << tablet_path
                << ". err: " << e.what();
            LOG(WARNING) << ss.str();
            return Status(ss.str());
        }

        // copy files one by one
        for (auto& file : snapshot_files) {
            std::string full_src_path = snapshot_path + "/" + file;
            std::string full_dest_path = tablet_path + "/" + file;
            RETURN_IF_ERROR(FileUtils::copy_file(full_src_path, full_dest_path));
            VLOG(2) << "copy file from " << full_src_path<< " to " << full_dest_path;
        }

    } else {
        // This is not a overwrite move
        // The files in tablet dir should be like this:
        //
        //  10001.hdr
        //  10001_0_70_3286516299297662422_0.idx
        //  10001_0_70_3286516299297662422_0.dat
        //  10001_71_71_4684061214850851594_0.idx
        //  10001_71_71_4684061214850851594_0.dat
        //  ...
        //
        //  0-70 version is supposed to be the placeholder version
        //
        // The files in snapshot dir should be like this:
        //  10001.hdr
        //  10001_0_40_4684061214850851594_0.idx
        //  10001_0_40_4684061214850851594_0.dat
        //  10001_41_68_1097018054900466785_0.idx
        //  10001_41_68_1097018054900466785_0.dat
        //  10001_69_69_8126494056407230455_0.idx
        //  10001_69_69_8126494056407230455_0.dat
        //  10001_70_70_6330898043876688539_0.idx
        //  10001_70_70_6330898043876688539_0.dat
        //  10001_71_71_0_0.idx
        //  10001_71_71_0_0.dat
        //
        //  71-71 may be exist as the palceholder version
        //
        // We need to move 0-70 version files from snapshot dir to
        // replace the 0-70 placeholder version in tablet dir.
        // than we merge the 2 .hdr file before reloading it.
    
        // load header in tablet dir to get the base vesion
        OLAPTablePtr tablet = OLAPEngine::get_instance()->get_table(
                tablet_id, schema_hash);
        if (tablet.get() == NULL) {
            std::stringstream ss;
            ss << "failed to get tablet: " << tablet_id << ", schema hash: "
                << schema_hash;
            LOG(WARNING) << ss.str();
            return Status(ss.str());
        }
        // get base version
        tablet->obtain_header_rdlock();
        const PDelta* base_version = tablet->base_version();
        tablet->release_header_lock();
        if (base_version == nullptr) {
            std::stringstream ss;
            ss << "failed to get base version of tablet: " << tablet_id;
            LOG(WARNING) << ss.str();
            return Status(ss.str());
        }
    
        int32_t end_version = base_version->end_version();

        // load snapshot tablet
        std::stringstream hdr;
        hdr << snapshot_path << "/" << tablet_id << ".hdr";
        std::string snapshot_header_file = hdr.str();
    
        OLAPHeader snapshot_header(snapshot_header_file);
        OLAPStatus ost = snapshot_header.load_and_init();
        if (ost != OLAP_SUCCESS) {
            LOG(WARNING) << "failed to load snapshot header: " << snapshot_header_file;
            return Status("failed to load snapshot header: " + snapshot_header_file);
        }

        LOG(INFO) << "begin to move snapshot files from version 0 to "
                << end_version << ", tablet id: " << tablet_id;

        // begin to move
        try {
            // delete the placeholder version in tablet dir
            std::string dummy;
            std::string place_holder_idx;
            _assemble_file_name("", tablet_path, tablet_id,
                    0, end_version,
                    base_version->version_hash(), 0, ".idx",
                    &dummy, &place_holder_idx);
            boost::filesystem::remove(place_holder_idx);

            std::string place_holder_dat;
            _assemble_file_name("", tablet_path, tablet_id,
                    0, end_version,
                    base_version->version_hash(), 0, ".dat",
                    &dummy, &place_holder_idx);
            boost::filesystem::remove(place_holder_dat);

            // copy files
            int version_size = snapshot_header.file_version_size();
            for (int i = 0; i < version_size; ++i) {
                const FileVersionMessage& version = snapshot_header.file_version(i);
                if (version.start_version() > end_version) {
                    continue;
                }
                int seg_num = version.num_segments();
                for (int j = 0; j < seg_num; i++) {
                    // idx
                    std::string idx_from;
                    std::string idx_to;
                    _assemble_file_name(snapshot_path, tablet_path, tablet_id,
                            version.start_version(), version.end_version(),
                            version.version_hash(), j, ".idx",
                            &idx_from, &idx_to);

                    boost::filesystem::copy_file(idx_from, idx_to);

                    // dat
                    std::string dat_from;
                    std::string dat_to;
                    _assemble_file_name(snapshot_path, tablet_path, tablet_id,
                            version.start_version(), version.end_version(),
                            version.version_hash(), j, ".dat",
                            &dat_from, &dat_to);
                    boost::filesystem::copy_file(dat_from, dat_to);
                }
            }
        } catch (const boost::filesystem::filesystem_error& e) {
            std::stringstream ss;
            ss << "failed to move tablet path: " << tablet_path
                << ". err: " << e.what();
            LOG(WARNING) << ss.str();
            return Status(ss.str());
        }

        // merge 2 headers
        ost = tablet->merge_header(snapshot_header, end_version);
        if (ost != OLAP_SUCCESS) {
            std::stringstream ss;
            ss << "failed to move tablet path: " << tablet_path;
            LOG(WARNING) << ss.str();
            return Status(ss.str());
        }
    }

    // fixme: there is no header now and can not call load_one_tablet here
    // reload header
    OlapStore* store = OLAPEngine::get_instance()->get_store(store_path);
    if (store == nullptr) {
        std::stringstream ss;
        ss << "failed to get store by path: " << store_path;
        LOG(WARNING) << ss.str();
        return Status(ss.str());
    }
    OLAPStatus ost = OLAPEngine::get_instance()->load_one_tablet(
            store, tablet_id, schema_hash, tablet_path, true);
    if (ost != OLAP_SUCCESS) {
        std::stringstream ss;
        ss << "failed to reload header of tablet: " << tablet_id;
        LOG(WARNING) << ss.str();
        return Status(ss.str());
    }
    LOG(INFO) << "finished to reload header of tablet: " << tablet_id;

    return status;
}

bool SnapshotLoader::_end_with(
    const std::string& str,
    const std::string& match) {

    if(str.size() >= match.size() &&
        str.compare(str.size() - match.size(), match.size(), match) == 0) {
        return true;
    }
    return false;
}

Status SnapshotLoader::_get_tablet_id_and_schema_hash_from_file_path(
        const std::string& src_path, int64_t* tablet_id, int32_t* schema_hash) {
    // path should be like: /path/.../tablet_id/schema_hash  
    // we try to extract tablet_id from path
    size_t pos = src_path.find_last_of("/");
    if (pos == std::string::npos || pos == src_path.length() - 1) {
        return Status("failed to get tablet id from path: " + src_path);
    }

    std::string schema_hash_str = src_path.substr(pos + 1);
    std::stringstream ss1;
    ss1 << schema_hash_str;
    ss1 >> *schema_hash;

    // skip schema hash part
    size_t pos2 = src_path.find_last_of("/", pos - 1);
    if (pos2 == std::string::npos) {
        return Status("failed to get tablet id from path: " + src_path);
    }

    std::string tablet_str = src_path.substr(pos2 + 1, pos - pos2);
    std::stringstream ss2;
    ss2 << tablet_str;
    ss2 >> *tablet_id;

    VLOG(2) << "get tablet id " << *tablet_id
            << ", schema hash: " << *schema_hash
            << " from path: " << src_path;
    return Status::OK;
}

Status SnapshotLoader::_check_local_snapshot_paths(
        const std::map<std::string, std::string>& src_to_dest_path, bool check_src) {
    for (const auto& pair : src_to_dest_path) {
        std::string path;
        if (check_src) {
            path = pair.first;
        } else {
            path = pair.second;
        }
        if (!FileUtils::is_dir(path)) {
            std::stringstream ss;
            ss << "snapshot path is not directory or does not exist: " << path;
            LOG(WARNING) << ss.str();
            return Status(TStatusCode::RUNTIME_ERROR, ss.str(), true);
        }
    }
    LOG(INFO) << "all local snapshot paths are existing. num: " << src_to_dest_path.size();
    return Status::OK;
}

Status SnapshotLoader::_get_existing_files_from_remote(
    BrokerServiceConnection& client,
    const std::string& remote_path,
    const std::map<std::string, std::string>& broker_prop,
    std::map<std::string, FileStat>* files) {
    try {
        // get existing files from remote path
        TBrokerListResponse list_rep;
        TBrokerListPathRequest list_req;
        list_req.__set_version(TBrokerVersion::VERSION_ONE);
        list_req.__set_path(remote_path + "/*");
        list_req.__set_isRecursive(false);
        list_req.__set_properties(broker_prop);
        list_req.__set_fileNameOnly(true);  // we only need file name, not abs path

        try {
            client->listPath(list_rep, list_req);
        } catch (apache::thrift::transport::TTransportException& e) {
            RETURN_IF_ERROR(client.reopen());
            client->listPath(list_rep, list_req);
        }

        if (list_rep.opStatus.statusCode == TBrokerOperationStatusCode::FILE_NOT_FOUND) {
            LOG(INFO) << "path does not exist: " << remote_path;
            return Status::OK;
        } else if (list_rep.opStatus.statusCode != TBrokerOperationStatusCode::OK) {
            std::stringstream ss;
            ss << "failed to list files from remote path: " << remote_path << ", msg: "
               << list_rep.opStatus.message;
            LOG(WARNING) << ss.str();
            return Status(ss.str());
        }
        LOG(INFO) << "finished to list files from remote path. file num: "
            << list_rep.files.size();

        // split file name and checksum
        for (const auto& file : list_rep.files) {
            if (file.isDir) {
                // this is not a file
                continue;
            }

            const std::string& file_name = file.path;
            size_t pos = file_name.find_last_of(".");
            if (pos == std::string::npos || pos == file_name.size() - 1) {
                // Not found checksum separator, ignore this file
                continue;
            }

            FileStat stat = { std::string(file_name, 0, pos), std::string(file_name, pos + 1), file.size };
            files->emplace(std::string(file_name, 0, pos), stat);
            VLOG(2) << "split remote file: " << std::string(file_name, 0, pos) << ", checksum: "
                    << std::string(file_name, pos + 1);
        }

        LOG(INFO) << "finished to split files. valid file num: "
            << files->size();

    } catch (apache::thrift::TException& e) {
        std::stringstream ss;
        ss << "failed to list files in remote path: " << remote_path << ", msg: " << e.what();
        LOG(WARNING) << ss.str();
        return Status(TStatusCode::THRIFT_RPC_ERROR, ss.str(), false);
    }

    return Status::OK;
}

Status SnapshotLoader::_get_existing_files_from_local(
    const std::string& local_path,
    std::vector<std::string>* local_files) {

    Status status = FileUtils::scan_dir(local_path, local_files);
    if (!status.ok()) {
        std::stringstream ss;
        ss << "failed to list files in local path: " << local_path << ", msg: "
           << status.get_error_msg();
        LOG(WARNING) << ss.str();
        return status;
    }
    LOG(INFO) << "finished to list files in local path: " << local_path << ", file num: "
              << local_files->size();
    return Status::OK;
}

Status SnapshotLoader::_rename_remote_file(
    BrokerServiceConnection& client,
    const std::string& orig_name,
    const std::string& new_name,
    const std::map<std::string, std::string>& broker_prop) {
    try {
        TBrokerOperationStatus op_status;
        TBrokerRenamePathRequest rename_req;
        rename_req.__set_version(TBrokerVersion::VERSION_ONE);
        rename_req.__set_srcPath(orig_name);
        rename_req.__set_destPath(new_name);
        rename_req.__set_properties(broker_prop);

        try {
            client->renamePath(op_status, rename_req);
        } catch (apache::thrift::transport::TTransportException& e) {
            RETURN_IF_ERROR(client.reopen());
            client->renamePath(op_status, rename_req);
        }

        if (op_status.statusCode != TBrokerOperationStatusCode::OK) {
            std::stringstream ss;
            ss << "Fail to rename file: " << orig_name << " to: " << new_name
                << " msg:" << op_status.message;
            LOG(WARNING) << ss.str();
            return Status(ss.str());
        }
    } catch (apache::thrift::TException& e) {
        std::stringstream ss;
        ss << "Fail to rename file: " << orig_name << " to: " << new_name
            << " msg:" << e.what();
        LOG(WARNING) << ss.str();
        return Status(TStatusCode::THRIFT_RPC_ERROR, ss.str(), false);
    }

    LOG(INFO) << "finished to rename file. orig: " << orig_name
            << ", new: " << new_name;

    return Status::OK;
}

void SnapshotLoader::_assemble_file_name(
    const std::string& snapshot_path,
    const std::string& tablet_path,
    int64_t tablet_id,
    int64_t start_version, int64_t end_version,
    int64_t vesion_hash, int32_t seg_num,
    const std::string suffix,
    std::string* snapshot_file, std::string* tablet_file) {

    std::stringstream ss1;
    ss1 << snapshot_path << "/" << tablet_id << "_"
        << start_version << "_" << end_version << "_"
        << vesion_hash << "_" << seg_num << suffix;
    *snapshot_file = ss1.str();

    std::stringstream ss2;
    ss2 << tablet_path << "/" << tablet_id << "_"
        << start_version << "_" << end_version << "_"
        << vesion_hash << "_" << seg_num << suffix;
    *tablet_file = ss2.str();

    VLOG(2) << "assemble file name: " << *snapshot_file
        << ", " << *tablet_file;
}

Status SnapshotLoader::_replace_tablet_id(
    const std::string& file_name,
    int64_t tablet_id,
    std::string* new_file_name) {

    // eg:
    // 10007.hdr
    // 10007_2_2_0_0.idx
    // 10007_2_2_0_0.dat
    if (_end_with(file_name, ".hdr")) {
        std::stringstream ss;
        ss << tablet_id << ".hdr";
        *new_file_name = ss.str();
        return Status::OK;
    } else if (_end_with(file_name, ".idx")
            || _end_with(file_name, ".dat")) {
        size_t pos = file_name.find_first_of("_");
        if (pos == std::string::npos) {
            return Status("invalid tablet file name: " + file_name);
        }

        std::string suffix_part = file_name.substr(pos);
        std::stringstream ss;
        ss << tablet_id << suffix_part;
        *new_file_name = ss.str();
        return Status::OK;
    } else {
        return Status("invalid tablet file name: " + file_name);
    }
}

Status SnapshotLoader::_get_tablet_id_from_remote_path(
    const std::string& remote_path,
    int64_t* tablet_id) {
    
    // eg:
    // bos://xxx/../__tbl_10004/__part_10003/__idx_10004/__10005
    size_t pos = remote_path.find_last_of("_");
    if (pos == std::string::npos) {
        return Status("invalid remove file path: " + remote_path);
    }

    std::string tablet_id_str = remote_path.substr(pos + 1);
    std::stringstream ss;
    ss << tablet_id_str;
    ss >> *tablet_id;

    return Status::OK;
}

} // end namespace doris