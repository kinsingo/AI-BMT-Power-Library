/**
 * @file device/soc_jetson.h
 * @brief NVIDIA Jetson SoC energy monitoring backend via INA3221 sensor.
 *
 * Reference: zeus/device/soc/jetson.py
 *
 * Reads power from INA3221 power monitor channels on Jetson boards:
 *   /sys/bus/i2c/drivers/ina3221x/{device}/{subdevice}/
 *
 * Power rails detected:
 *   - CPU power (rail names containing "cpu")
 *   - GPU power (rail names containing "gpu")
 *   - Total/system power (rail names containing "system", "_in", or "total")
 *
 * Background polling thread integrates instantaneous power → cumulative energy.
 *
 * Linux only. On non-Linux platforms, the constructor throws std::runtime_error.
 */

#pragma once

#ifdef __linux__
  #include <dirent.h>
  #include <sys/stat.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace zeus {

/**
 * @brief NVIDIA Jetson SoC energy backend via INA3221 sensor.
 *
 * Metrics stored in soc_energy with keys:
 *   "jetson_cpu", "jetson_gpu", "jetson_total"
 *
 * On non-Linux or non-Jetson platforms, the constructor throws.
 */
class JetsonSoCBackend {
public:
    static bool is_compiled() {
#ifdef __linux__
        return true;
#else
        return false;
#endif
    }

    static bool is_available() {
#ifdef __linux__
        struct stat st;
        bool has_tegra =
            (stat("/usr/lib/aarch64-linux-gnu/tegra", &st) == 0) ||
            (stat("/etc/nv_tegra_release", &st) == 0);
        if (!has_tegra) return false;
        // Also check INA3221 driver exists
        return (stat("/sys/bus/i2c/drivers/ina3221x", &st) == 0);
#else
        return false;
#endif
    }

    // ---- Construction ----

    explicit JetsonSoCBackend(double polling_interval_s = 0.1)
        : polling_active_(false)
        , polling_interval_(polling_interval_s)
    {
#ifdef __linux__
        if (!is_available()) {
            throw std::runtime_error(
                "Jetson SoC monitoring requested but no Jetson device detected. "
                "Requires /usr/lib/aarch64-linux-gnu/tegra or /etc/nv_tegra_release "
                "and /sys/bus/i2c/drivers/ina3221x.");
        }
        discover_power_rails();
        if (power_rails_.empty()) {
            throw std::runtime_error(
                "Jetson SoC detected but no INA3221 power rails found.");
        }
        // Initialize cumulative energy for each discovered rail
        for (const auto& rail : power_rails_) {
            cumulative_energy_mj_[rail.key] = 0.0;
        }
        start_polling();
#else
        (void)polling_interval_s;
        throw std::runtime_error(
            "Jetson SoC monitoring requires Linux (aarch64 Jetson platform).");
#endif
    }

    ~JetsonSoCBackend() { stop_polling(); }

    JetsonSoCBackend(const JetsonSoCBackend&) = delete;
    JetsonSoCBackend& operator=(const JetsonSoCBackend&) = delete;

    // ---- Query methods ----

    /** Return available SoC metric keys (e.g., "jetson_cpu", "jetson_gpu"). */
    std::set<std::string> available_metrics() const {
        std::set<std::string> metrics;
        for (const auto& rail : power_rails_) {
            metrics.insert(rail.key);
        }
        return metrics;
    }

    /**
     * Snapshot current cumulative energy (Joules) for all rails.
     * Thread-safe — reads from the polling thread's accumulator.
     */
    std::map<std::string, double> snapshot_energy_j() const {
        std::lock_guard<std::mutex> lock(energy_mutex_);
        std::map<std::string, double> snap;
        for (const auto& kv : cumulative_energy_mj_) {
            snap[kv.first] = kv.second / 1000.0;  // mJ -> J
        }
        return snap;
    }

    /**
     * Compute SoC energy delta (Joules) from a start snapshot to now.
     */
    std::map<std::string, double> compute_energy_delta_j(
        const std::map<std::string, double>& start_snap) const
    {
        auto end_snap = snapshot_energy_j();
        std::map<std::string, double> result;
        for (const auto& kv : end_snap) {
            auto it = start_snap.find(kv.first);
            double start_e = (it != start_snap.end()) ? it->second : 0.0;
            result[kv.first] = kv.second - start_e;
        }
        return result;
    }

    /**
     * Get instantaneous power (Watts) for a specific rail key.
     * Reads directly from INA3221 sysfs sensor.
     *
     * @param metric  Rail key (e.g., "jetson_cpu", "jetson_gpu", "jetson_total")
     */
    double get_instant_power_w(const std::string& metric) const {
        for (const auto& rail : power_rails_) {
            if (rail.key == metric) {
                return rail.read_power_mw() / 1000.0;  // mW -> W
            }
        }
        throw std::runtime_error(
            "Jetson SoC: unknown metric '" + metric + "' for power query");
    }

private:
    // ---- Power rail representation ----

    enum class PowerStrategy { DirectPower, VoltageCurrentProduct };

    struct PowerRail {
        std::string key;           ///< "jetson_cpu", "jetson_gpu", "jetson_total"
        PowerStrategy strategy;
        std::string power_path;    ///< sysfs path for direct power (mW)
        std::string voltage_path;  ///< sysfs path for voltage (mV)
        std::string current_path;  ///< sysfs path for current (mA)

        /** Read instantaneous power in mW. */
        double read_power_mw() const {
            if (strategy == PowerStrategy::DirectPower) {
                return read_sysfs_double(power_path);
            } else {
                double v = read_sysfs_double(voltage_path);
                double c = read_sysfs_double(current_path);
                return (v * c) / 1000.0;  // mV * mA / 1000 = mW
            }
        }
    };

    std::vector<PowerRail> power_rails_;

    // ---- Cumulative energy (thread-safe) ----

    mutable std::mutex energy_mutex_;
    std::map<std::string, double> cumulative_energy_mj_;

    // ---- Polling ----

    std::thread polling_thread_;
    std::atomic<bool> polling_active_;
    double polling_interval_;

    void start_polling() {
        polling_active_ = true;
        polling_thread_ = std::thread([this]() {
            auto prev_ts = std::chrono::steady_clock::now();
            while (polling_active_.load()) {
                auto now = std::chrono::steady_clock::now();
                double dt = std::chrono::duration<double>(now - prev_ts).count();
                prev_ts = now;

                std::lock_guard<std::mutex> lock(energy_mutex_);
                for (const auto& rail : power_rails_) {
                    try {
                        double power_mw = rail.read_power_mw();
                        // energy (mJ) = power (mW) * time (s)
                        cumulative_energy_mj_[rail.key] += power_mw * dt;
                    } catch (...) {}
                }

                std::this_thread::sleep_for(
                    std::chrono::duration<double>(polling_interval_));
            }
        });
    }

    void stop_polling() {
        if (polling_active_.load()) {
            polling_active_ = false;
            if (polling_thread_.joinable()) polling_thread_.join();
        }
    }

    // ---- sysfs helpers ----

    static double read_sysfs_double(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) {
            throw std::runtime_error("Cannot read sysfs: " + path);
        }
        double val = 0.0;
        f >> val;
        return val;
    }

    static bool file_exists(const std::string& path) {
#ifdef __linux__
        struct stat st;
        return stat(path.c_str(), &st) == 0;
#else
        (void)path;
        return false;
#endif
    }

    static std::string read_sysfs_string(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return "";
        std::string val;
        std::getline(f, val);
        // Trim trailing whitespace
        while (!val.empty() && (val.back() == '\n' || val.back() == '\r' || val.back() == ' '))
            val.pop_back();
        return val;
    }

    /**
     * Classify a rail name to a jetson metric key.
     * Returns "" if the rail is not a recognized type.
     */
    static std::string classify_rail(const std::string& rail_name) {
        std::string lower = rail_name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower.find("cpu") != std::string::npos) return "jetson_cpu";
        if (lower.find("gpu") != std::string::npos) return "jetson_gpu";
        if (lower.find("system") != std::string::npos ||
            lower.find("_in") != std::string::npos ||
            lower.find("total") != std::string::npos) return "jetson_total";
        return "";
    }

    // ---- Discovery ----

#ifdef __linux__
    void discover_power_rails() {
        const std::string base = "/sys/bus/i2c/drivers/ina3221x";
        DIR* base_dir = opendir(base.c_str());
        if (!base_dir) return;

        // Track which keys we've already found (avoid duplicates)
        std::set<std::string> found_keys;

        struct dirent* device_entry;
        while ((device_entry = readdir(base_dir)) != nullptr) {
            std::string device_name = device_entry->d_name;
            if (device_name == "." || device_name == ".." ||
                device_name == "bind" || device_name == "unbind" ||
                device_name == "module") continue;

            std::string device_path = base + "/" + device_name;
            DIR* device_dir = opendir(device_path.c_str());
            if (!device_dir) continue;

            struct dirent* sub_entry;
            while ((sub_entry = readdir(device_dir)) != nullptr) {
                std::string sub_name = sub_entry->d_name;
                if (sub_name == "." || sub_name == "..") continue;

                std::string sub_path = device_path + "/" + sub_name;
                discover_from_labels(sub_path, found_keys);
                discover_from_rail_names(sub_path, found_keys);
            }
            closedir(device_dir);
        }
        closedir(base_dir);
    }

    /**
     * Discover power rails from in*_label files.
     * Pattern: in{N}_label -> rail name, power{N}_input / in{N}_input + curr{N}_input
     */
    void discover_from_labels(const std::string& path, std::set<std::string>& found) {
        DIR* dir = opendir(path.c_str());
        if (!dir) return;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string fname = entry->d_name;

            // Match pattern: in{N}_label
            if (fname.size() < 3 || fname.substr(0, 2) != "in") continue;
            auto label_pos = fname.find("_label");
            if (label_pos == std::string::npos) continue;

            std::string idx_part = fname.substr(2, label_pos - 2);
            std::string rail_name = read_sysfs_string(path + "/" + fname);
            if (rail_name.empty()) continue;

            std::string key = classify_rail(rail_name);
            if (key.empty() || found.count(key)) continue;

            // Try direct power file
            std::string power_path = path + "/power" + idx_part + "_input";
            std::string volt_path  = path + "/in" + idx_part + "_input";
            std::string curr_path  = path + "/curr" + idx_part + "_input";

            if (file_exists(power_path)) {
                power_rails_.push_back({key, PowerStrategy::DirectPower,
                                        power_path, "", ""});
                found.insert(key);
            } else if (file_exists(volt_path) && file_exists(curr_path)) {
                power_rails_.push_back({key, PowerStrategy::VoltageCurrentProduct,
                                        "", volt_path, curr_path});
                found.insert(key);
            }
        }
        closedir(dir);
    }

    /**
     * Discover power rails from rail_name_* files.
     * Pattern: rail_name_{N} -> rail name, in_power{N}_input / in_voltage{N}_input + in_current{N}_input
     */
    void discover_from_rail_names(const std::string& path, std::set<std::string>& found) {
        DIR* dir = opendir(path.c_str());
        if (!dir) return;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string fname = entry->d_name;

            // Match pattern: rail_name_{N}
            const std::string prefix = "rail_name_";
            if (fname.size() <= prefix.size() || fname.substr(0, prefix.size()) != prefix) continue;

            std::string idx_part = fname.substr(prefix.size());
            std::string rail_name = read_sysfs_string(path + "/" + fname);
            if (rail_name.empty()) continue;

            std::string key = classify_rail(rail_name);
            if (key.empty() || found.count(key)) continue;

            // Try direct power file
            std::string power_path = path + "/in_power" + idx_part + "_input";
            std::string volt_path  = path + "/in_voltage" + idx_part + "_input";
            std::string curr_path  = path + "/in_current" + idx_part + "_input";

            if (file_exists(power_path)) {
                power_rails_.push_back({key, PowerStrategy::DirectPower,
                                        power_path, "", ""});
                found.insert(key);
            } else if (file_exists(volt_path) && file_exists(curr_path)) {
                power_rails_.push_back({key, PowerStrategy::VoltageCurrentProduct,
                                        "", volt_path, curr_path});
                found.insert(key);
            }
        }
        closedir(dir);
    }
#endif // __linux__
};

} // namespace zeus
