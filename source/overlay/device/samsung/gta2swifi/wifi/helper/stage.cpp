// Fixed EFS-to-tmpfs identity reader; no EFS writes or Wi-Fi actuation.
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "mac.h"

namespace {
using android::base::unique_fd;
int Fail(const char* stage) {
    LOG(ERROR) << "Wi-Fi identity staging failed at " << stage;
    return 1;
}
}

int main(int argc, char** argv) {
    (void)argc;
    android::base::InitLogging(argv, android::base::LogdLogger());
    // Init continues after exec_start errors. Never authorize preparation after
    // a failed stage or merely because a preexisting RAM file parses as a MAC.
    constexpr char kStaged[] = "vendor.t380.wifi.identity_staged";
    if (!android::base::SetProperty(kStaged, "0")) return Fail("property reset");
    // O_PATH needs directory search only: EFS's observed root mode is 0711.
    unique_fd efs(open("/mnt/vendor/efs", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!efs.ok()) return Fail("EFS directory");
    unique_fd wifi(openat(efs.get(), "wifi", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!wifi.ok()) return Fail("Wi-Fi directory");
    unique_fd source(openat(wifi.get(), ".mac.info", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (!source.ok()) return Fail("factory source");
    struct stat source_stat {};
    struct statfs source_fs {};
    struct statvfs source_vfs {};
    if (fstat(source.get(), &source_stat) != 0 || !S_ISREG(source_stat.st_mode) ||
        (source_stat.st_size != 17 && source_stat.st_size != 18) ||
        fstatfs(source.get(), &source_fs) != 0 || source_fs.f_type != EXT4_SUPER_MAGIC ||
        fstatvfs(source.get(), &source_vfs) != 0 || !(source_vfs.f_flag & ST_RDONLY))
        return Fail("read-only factory file");
    char buffer[19];
    const ssize_t size = TEMP_FAILURE_RETRY(read(source.get(), buffer, sizeof(buffer)));
    if (size != source_stat.st_size) return Fail("factory length");
    char extra;
    if (TEMP_FAILURE_RETRY(read(source.get(), &extra, 1)) != 0) return Fail("factory trailing data");
    std::string mac(buffer, static_cast<size_t>(size));
    if (!t380wifi::Mac(&mac)) return Fail("factory format");

    unique_fd destination(open("/mnt/vendor/t380-wifi", O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!destination.ok()) return Fail("RAM directory");
    struct stat destination_stat {};
    struct statfs destination_fs {};
    if (fstat(destination.get(), &destination_stat) != 0 ||
        destination_stat.st_uid != getuid() || destination_stat.st_gid != getgid() ||
        (destination_stat.st_mode & 07777) != 0700 ||
        fstatfs(destination.get(), &destination_fs) != 0 || destination_fs.f_type != TMPFS_MAGIC)
        return Fail("RAM filesystem");
    // Do not follow, replace or trust any preexisting identity file.
    umask(0077);
    unique_fd output(openat(destination.get(), "factory-mac",
                           O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (!output.ok()) return Fail("RAM creation");
    if (!android::base::WriteStringToFd(mac, output.get())) {
        unlinkat(destination.get(), "factory-mac", 0);
        return Fail("RAM write");
    }
    if (!android::base::SetProperty(kStaged, "1")) {
        unlinkat(destination.get(), "factory-mac", 0);
        return Fail("stage authorization");
    }
    LOG(INFO) << "Wi-Fi factory identity staged in protected RAM";
    return 0;
}
