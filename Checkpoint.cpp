/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "Checkpoint"
#include "Checkpoint.h"
#include "VoldUtil.h"

#include <fstream>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <android/hardware/boot/1.0/IBootControl.h>
#include <cutils/android_reboot.h>
#include <fcntl.h>
#include <fs_mgr.h>
#include <linux/fs.h>
#include <mntent.h>
#include <sys/mount.h>
#include <sys/stat.h>

using android::base::SetProperty;
using android::binder::Status;
using android::fs_mgr::Fstab;
using android::fs_mgr::ReadDefaultFstab;
using android::fs_mgr::ReadFstabFromFile;
using android::hardware::hidl_string;
using android::hardware::boot::V1_0::BoolResult;
using android::hardware::boot::V1_0::IBootControl;
using android::hardware::boot::V1_0::Slot;

namespace android {
namespace vold {

namespace {
const std::string kMetadataCPFile = "/metadata/vold/checkpoint";

bool setBowState(std::string const& block_device, std::string const& state) {
    if (block_device.substr(0, 5) != "/dev/") {
        LOG(ERROR) << "Expected block device, got " << block_device;
        return false;
    }

    std::string state_filename = std::string("/sys/") + block_device.substr(5) + "/bow/state";
    if (!android::base::WriteStringToFile(state, state_filename)) {
        PLOG(ERROR) << "Failed to write to file " << state_filename;
        return false;
    }

    return true;
}

}  // namespace

Status cp_supportsCheckpoint(bool& result) {
    result = false;

    for (const auto& entry : fstab_default) {
        if (entry.fs_mgr_flags.checkpoint_blk || entry.fs_mgr_flags.checkpoint_fs) {
            result = true;
            return Status::ok();
        }
    }
    return Status::ok();
}

Status cp_startCheckpoint(int retry) {
    if (retry < -1) return Status::fromExceptionCode(EINVAL, "Retry count must be more than -1");
    std::string content = std::to_string(retry + 1);
    if (retry == -1) {
        sp<IBootControl> module = IBootControl::getService();
        if (module) {
            std::string suffix;
            auto cb = [&suffix](hidl_string s) { suffix = s; };
            if (module->getSuffix(module->getCurrentSlot(), cb).isOk()) content += " " + suffix;
        }
    }
    if (!android::base::WriteStringToFile(content, kMetadataCPFile))
        return Status::fromExceptionCode(errno, "Failed to write checkpoint file");
    return Status::ok();
}

namespace {

bool isCheckpointing = false;
}

Status cp_commitChanges() {
    if (!isCheckpointing) {
        return Status::ok();
    }
    // Must take action for list of mounted checkpointed things here
    // To do this, we walk the list of mounted file systems.
    // But we also need to get the matching fstab entries to see
    // the original flags
    std::string err_str;

    Fstab mounts;
    if (!ReadFstabFromFile("/proc/mounts", &mounts)) {
        return Status::fromExceptionCode(EINVAL, "Failed to get /proc/mounts");
    }

    // Walk mounted file systems
    for (const auto& mount_rec : mounts) {
        const auto fstab_rec = GetEntryForMountPoint(&fstab_default, mount_rec.mount_point);
        if (!fstab_rec) continue;

        if (fstab_rec->fs_mgr_flags.checkpoint_fs) {
            if (fstab_rec->fs_type == "f2fs") {
                std::string options = mount_rec.fs_options + ",checkpoint=enable";
                if (mount(mount_rec.blk_device.c_str(), mount_rec.mount_point.c_str(), "none",
                          MS_REMOUNT | fstab_rec->flags, options.c_str())) {
                    return Status::fromExceptionCode(EINVAL, "Failed to remount");
                }
            }
        } else if (fstab_rec->fs_mgr_flags.checkpoint_blk) {
            if (!setBowState(mount_rec.blk_device, "2"))
                return Status::fromExceptionCode(EINVAL, "Failed to set bow state");
        }
    }
    SetProperty("vold.checkpoint_committed", "1");
    isCheckpointing = false;
    if (!android::base::RemoveFileIfExists(kMetadataCPFile, &err_str))
        return Status::fromExceptionCode(errno, err_str.c_str());
    return Status::ok();
}

Status cp_abortChanges() {
    android_reboot(ANDROID_RB_RESTART2, 0, nullptr);
    return Status::ok();
}

bool cp_needsRollback() {
    std::string content;
    bool ret;

    ret = android::base::ReadFileToString(kMetadataCPFile, &content);
    if (ret) {
        if (content == "0") return true;
        if (content.substr(0, 3) == "-1 ") {
            std::string oldSuffix = content.substr(3);
            sp<IBootControl> module = IBootControl::getService();
            std::string newSuffix;

            if (module) {
                auto cb = [&newSuffix](hidl_string s) { newSuffix = s; };
                module->getSuffix(module->getCurrentSlot(), cb);
                if (oldSuffix == newSuffix) return true;
            }
        }
    }
    return false;
}

bool cp_needsCheckpoint() {
    bool ret;
    std::string content;
    sp<IBootControl> module = IBootControl::getService();

    if (module && module->isSlotMarkedSuccessful(module->getCurrentSlot()) == BoolResult::FALSE) {
        isCheckpointing = true;
        return true;
    }
    ret = android::base::ReadFileToString(kMetadataCPFile, &content);
    if (ret) {
        ret = content != "0";
        isCheckpointing = ret;
        return ret;
    }
    return false;
}

Status cp_prepareCheckpoint() {
    Fstab mounts;
    if (!ReadFstabFromFile("/proc/mounts", &mounts)) {
        return Status::fromExceptionCode(EINVAL, "Failed to get /proc/mounts");
    }

    for (const auto& mount_rec : mounts) {
        const auto fstab_rec = GetEntryForMountPoint(&fstab_default, mount_rec.mount_point);
        if (!fstab_rec) continue;

        if (fstab_rec->fs_mgr_flags.checkpoint_blk) {
            android::base::unique_fd fd(
                TEMP_FAILURE_RETRY(open(mount_rec.mount_point.c_str(), O_RDONLY | O_CLOEXEC)));
            if (!fd) {
                PLOG(ERROR) << "Failed to open mount point" << mount_rec.mount_point;
                continue;
            }

            struct fstrim_range range = {};
            range.len = ULLONG_MAX;
            if (ioctl(fd, FITRIM, &range)) {
                PLOG(ERROR) << "Failed to trim " << mount_rec.mount_point;
                continue;
            }

            setBowState(mount_rec.blk_device, "1");
        }
    }
    return Status::ok();
}

namespace {
const int kBlockSize = 4096;
const int kSectorSize = 512;

typedef uint64_t sector_t;

struct log_entry {
    sector_t source;
    sector_t dest;
    uint32_t size;
    uint32_t checksum;
} __attribute__((packed));

struct log_sector {
    uint32_t magic;
    uint32_t count;
    uint32_t sequence;
    uint64_t sector0;
    struct log_entry entries[];
} __attribute__((packed));

// MAGIC is BOW in ascii
const int kMagic = 0x00574f42;

void crc32(const void* data, size_t n_bytes, uint32_t* crc) {
    static uint32_t table[0x100] = {
        0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535,
        0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD,
        0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D,
        0x6DDDE4EB, 0xF4D4B551, 0x83D385C7, 0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC,
        0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4,
        0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
        0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59, 0x26D930AC,
        0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
        0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB,
        0xB6662D3D,

        0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5,
        0xE8B8D433, 0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D,
        0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED,
        0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C,
        0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65, 0x4DB26158, 0x3AB551CE, 0xA3BC0074,
        0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC,
        0xAD678846, 0xDA60B8D0, 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9, 0x5005713C,
        0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
        0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B,
        0xC0BA6CAD,

        0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615,
        0x73DC1683, 0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D,
        0x0A00AE27, 0x7D079EB1, 0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D,
        0x806567CB, 0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
        0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4,
        0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B, 0xD80D2BDA, 0xAF0A1B4C,
        0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79, 0xCB61B38C,
        0xBC66831A, 0x256FD2A0, 0x5268E236, 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
        0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B,
        0x5BDEAE1D,

        0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F, 0x72076785,
        0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D,
        0x7CDCEFB7, 0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD,
        0xF6B9265B, 0x6FB077E1, 0x18B74777, 0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C,
        0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45, 0xA00AE278, 0xD70DD2EE, 0x4E048354,
        0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
        0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9, 0xBDBDF21C,
        0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
        0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94, 0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B,
        0x2D02EF8D};

    for (size_t i = 0; i < n_bytes; ++i) {
        *crc ^= ((uint8_t*)data)[i];
        *crc = table[(uint8_t)*crc] ^ *crc >> 8;
    }
}

}  // namespace

static void read(std::fstream& device, std::vector<log_entry> const& logs, sector_t sector,
                 char* buffer) {
    for (auto l = logs.rbegin(); l != logs.rend(); l++)
        if (sector >= l->source && (sector - l->source) * kSectorSize < l->size)
            sector = sector - l->source + l->dest;

    device.seekg(sector * kSectorSize);
    device.read(buffer, kBlockSize);
}

static std::vector<char> read(std::fstream& device, std::vector<log_entry> const& logs,
                              bool validating, sector_t sector, uint32_t size) {
    if (!validating) {
        std::vector<char> buffer(size);
        device.seekg(sector * kSectorSize);
        device.read(&buffer[0], size);
        return buffer;
    }

    // Crude approach at first where we do this sector by sector and just scan
    // the entire logs for remappings each time
    std::vector<char> buffer(size);

    for (uint32_t i = 0; i < size; i += kBlockSize, sector += kBlockSize / kSectorSize)
        read(device, logs, sector, &buffer[i]);

    return buffer;
}

Status cp_restoreCheckpoint(const std::string& blockDevice) {
    bool validating = true;
    std::string action = "Validating";

    for (;;) {
        std::vector<log_entry> logs;
        Status status = Status::ok();

        LOG(INFO) << action << " checkpoint on " << blockDevice;
        std::fstream device(blockDevice, std::ios::binary | std::ios::in | std::ios::out);
        if (!device) {
            PLOG(ERROR) << "Cannot open " << blockDevice;
            return Status::fromExceptionCode(errno, ("Cannot open " + blockDevice).c_str());
        }
        auto buffer = read(device, logs, validating, 0, kBlockSize);
        log_sector& ls = *reinterpret_cast<log_sector*>(&buffer[0]);
        if (ls.magic != kMagic) {
            LOG(ERROR) << "No magic";
            return Status::fromExceptionCode(EINVAL, "No magic");
        }

        LOG(INFO) << action << " " << ls.sequence << " log sectors";

        for (int sequence = ls.sequence; sequence >= 0 && status.isOk(); sequence--) {
            auto buffer = read(device, logs, validating, 0, kBlockSize);
            log_sector& ls = *reinterpret_cast<log_sector*>(&buffer[0]);
            if (ls.magic != kMagic) {
                LOG(ERROR) << "No magic!";
                status = Status::fromExceptionCode(EINVAL, "No magic");
                break;
            }

            if ((int)ls.sequence != sequence) {
                LOG(ERROR) << "Expecting log sector " << sequence << " but got " << ls.sequence;
                status = Status::fromExceptionCode(
                    EINVAL, ("Expecting log sector " + std::to_string(sequence) + " but got " +
                             std::to_string(ls.sequence))
                                .c_str());
                break;
            }

            LOG(INFO) << action << " from log sector " << ls.sequence;

            for (log_entry* le = &ls.entries[ls.count - 1]; le >= ls.entries; --le) {
                LOG(INFO) << action << " " << le->size << " bytes from sector " << le->dest
                          << " to " << le->source << " with checksum " << std::hex << le->checksum;
                auto buffer = read(device, logs, validating, le->dest, le->size);
                uint32_t checksum = le->source / (kBlockSize / kSectorSize);
                for (size_t i = 0; i < le->size; i += kBlockSize) {
                    crc32(&buffer[i], kBlockSize, &checksum);
                }

                if (le->checksum && checksum != le->checksum) {
                    LOG(ERROR) << "Checksums don't match " << std::hex << checksum;
                    status = Status::fromExceptionCode(EINVAL, "Checksums don't match");
                    break;
                }

                logs.push_back(*le);

                if (!validating) {
                    device.seekg(le->source * kSectorSize);
                    device.write(&buffer[0], le->size);
                }
            }
        }

        if (!status.isOk()) {
            if (!validating) {
                LOG(ERROR) << "Checkpoint restore failed even though checkpoint validation passed";
                return status;
            }

            LOG(WARNING) << "Checkpoint validation failed - attempting to roll forward";
            auto buffer = read(device, logs, false, ls.sector0, kBlockSize);
            device.seekg(0);
            device.write(&buffer[0], kBlockSize);
            return Status::ok();
        }

        if (!validating) break;

        validating = false;
        action = "Restoring";
    }

    return Status::ok();
}

Status cp_markBootAttempt() {
    std::string oldContent, newContent;
    int retry = 0;
    struct stat st;
    int result = stat(kMetadataCPFile.c_str(), &st);

    // If the file doesn't exist, we aren't managing a checkpoint retry counter
    if (result != 0) return Status::ok();
    if (!android::base::ReadFileToString(kMetadataCPFile, &oldContent)) {
        PLOG(ERROR) << "Failed to read checkpoint file";
        return Status::fromExceptionCode(errno, "Failed to read checkpoint file");
    }
    std::string retryContent = oldContent.substr(0, oldContent.find_first_of(" "));

    if (!android::base::ParseInt(retryContent, &retry))
        return Status::fromExceptionCode(EINVAL, "Could not parse retry count");
    if (retry > 0) {
        retry--;

        newContent = std::to_string(retry);
        if (!android::base::WriteStringToFile(newContent, kMetadataCPFile))
            return Status::fromExceptionCode(errno, "Could not write checkpoint file");
    }
    return Status::ok();
}

}  // namespace vold
}  // namespace android
