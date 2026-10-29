// Candidate only: real firmware completion and factory identity, no EFS writes.
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <chrono>
#include "mac.h"
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

namespace {
constexpr char kPlatform[] = "/sys/devices/platform/soc/a000000.qcom,wcnss-wlan/";
bool Read(const std::string& path, std::string* value) {
    android::base::unique_fd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!fd.ok()) return false;
    char buffer[64];
    const ssize_t size = TEMP_FAILURE_RETRY(read(fd, buffer, sizeof(buffer)));
    if (size < 0 || size == static_cast<ssize_t>(sizeof(buffer))) return false;
    char extra;
    if (TEMP_FAILURE_RETRY(read(fd, &extra, 1)) != 0) return false;
    value->assign(buffer, static_cast<size_t>(size));
    return true;
}
using t380wifi::Mac;
int Fail(const char* stage) {
    LOG(ERROR) << "Wi-Fi preparation failed at " << stage;  // No identity in logs.
    return 1;
}
}

int main(int argc, char** argv) {
    (void)argc;
    android::base::InitLogging(argv, android::base::LogdLogger());
    if (!android::base::SetProperty("vendor.t380.wifi.prepared", "0")) return Fail("property reset");
    // One attempt per boot; never reload a partially initialized driver.
    if (access("/sys/module/wlan", F_OK) == 0) return Fail("module already present");
    std::string mac;
    if (!Read("/mnt/vendor/t380-wifi/factory-mac", &mac) || !Mac(&mac)) return Fail("factory MAC");
    android::base::unique_fd mac_fd(open((std::string(kPlatform) + "wcnss_mac_addr").c_str(),
                                      O_WRONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!mac_fd.ok() || !android::base::WriteStringToFd(mac + "\n", mac_fd)) return Fail("MAC provision");
    mac_fd.reset();
    {  // Existing WCNSS open triggers firmware/NV work; no synthetic status writes.
        android::base::unique_fd trigger(open("/dev/wcnss_wlan", O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if (!trigger.ok()) return Fail("firmware trigger");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::string ready;
    do {
        if (!Read(std::string(kPlatform) + "wcnss_ready", &ready) ||
            (ready != "0\n" && ready != "1\n")) return Fail("readiness bridge");
        if (ready == "1\n") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
    if (ready != "1\n") return Fail("NV/CBC timeout");
    android::base::unique_fd module(open("/vendor/lib/modules/wlan.ko", O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!module.ok() || syscall(__NR_finit_module, module.get(), "", 0) != 0) return Fail("module insertion");
    std::string observed;
    if (!Read("/sys/class/net/wlan0/address", &observed) || !Mac(&observed) || observed != mac)
        return Fail("interface identity");
    if (!android::base::SetProperty("vendor.t380.wifi.prepared", "1")) return Fail("ready property");
    LOG(INFO) << "Wi-Fi firmware, NV/CBC, module and factory identity verified";
    return 0;
}
