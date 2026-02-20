/**
 * @file device/cpu_rapl.h
 * @brief Intel CPU/DRAM energy monitoring backend via Linux RAPL sysfs.
 *
 * Reference: zeus/device/cpu/rapl.py
 *
 * Reads cumulative energy from:
 *   /sys/class/powercap/intel-rapl/intel-rapl:{cpu}/energy_uj
 *   /sys/class/powercap/intel-rapl/intel-rapl:{cpu}/intel-rapl:{cpu}:{sub}/energy_uj
 *
 * All #ifdef guards are contained within this file.
 * The class RaplBackend always exists — on non-Linux platforms,
 * the constructor throws std::runtime_error.
 */

#pragma once

#ifdef __linux__
  #include <dirent.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace zeus {

/**
 * @brief Intel RAPL CPU/DRAM energy backend via Linux sysfs.
 *
 * On non-Linux platforms, is_compiled() returns false and the constructor
 * throws std::runtime_error. On Linux, the constructor auto-discovers
 * available CPU sockets and DRAM sub-packages.
 */
class RaplBackend {
public:
    struct Snapshot {
        std::map<int, double> cpu;   ///< CPU socket -> cumulative Joules
        std::map<int, double> dram;  ///< CPU socket -> cumulative Joules
    };

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
        return (stat("/sys/class/powercap/intel-rapl", &st) == 0) ||
               (stat("/zeus_sys/class/powercap/intel-rapl", &st) == 0);
#else
        return false;
#endif
    }

    // ---- Construction ----

    RaplBackend() {
#ifdef __linux__
        discover_cpus("/sys/class/powercap/intel-rapl");
        if (cpu_dirs_.empty()) {
            // Try Zeus container mount point
            discover_cpus("/zeus_sys/class/powercap/intel-rapl");
        }
        if (cpu_dirs_.empty()) {
            throw std::runtime_error(
                "Intel CPU (RAPL) monitoring requested but no RAPL sysfs entries found. "
                "Ensure the intel_rapl kernel module is loaded and you have read permissions.");
        }
#else
        throw std::runtime_error(
            "Intel CPU (RAPL) monitoring requires Linux. "
            "Current platform does not support RAPL sysfs.");
#endif
    }

    // ---- Query methods ----

    bool available() const { return !cpu_dirs_.empty(); }

    std::vector<int> cpu_indices() const {
        std::vector<int> indices;
        indices.reserve(cpu_dirs_.size());
        for (const auto& kv : cpu_dirs_) indices.push_back(kv.first);
        return indices;
    }

    bool supports_dram(int cpu_index) const {
        return dram_dirs_.count(cpu_index) > 0;
    }

    /** Max energy range (Joules) for a CPU socket's RAPL counter.
     *  Used to detect and correct counter wraparound. */
    double max_energy_range_j(int cpu_index) const {
        auto it = max_energy_j_.find(cpu_index);
        return (it != max_energy_j_.end()) ? it->second : 0.0;
    }

    /** Max energy range (Joules) for a DRAM RAPL counter. */
    double max_dram_energy_range_j(int cpu_index) const {
        auto it = max_dram_energy_j_.find(cpu_index);
        return (it != max_dram_energy_j_.end()) ? it->second : 0.0;
    }

    double read_cpu_energy_j(int cpu_index) const {
        return read_energy_uj(cpu_dirs_.at(cpu_index) + "/energy_uj") / 1e6;
    }

    double read_dram_energy_j(int cpu_index) const {
        auto it = dram_dirs_.find(cpu_index);
        if (it == dram_dirs_.end()) return 0.0;
        return read_energy_uj(it->second + "/energy_uj") / 1e6;
    }

    Snapshot take_snapshot() const {
        Snapshot snap;
        for (const auto& kv : cpu_dirs_) {
            snap.cpu[kv.first] = read_cpu_energy_j(kv.first);
            if (supports_dram(kv.first)) {
                snap.dram[kv.first] = read_dram_energy_j(kv.first);
            }
        }
        return snap;
    }

private:
    std::map<int, std::string> cpu_dirs_;       ///< cpu_idx -> sysfs path
    std::map<int, std::string> dram_dirs_;      ///< cpu_idx -> dram sysfs path
    std::map<int, double>      max_energy_j_;   ///< cpu_idx -> max counter range (J)
    std::map<int, double>      max_dram_energy_j_; ///< cpu_idx -> max DRAM counter range (J)

#ifdef __linux__
    void discover_cpus(const std::string& base) {
        DIR* dir = opendir(base.c_str());
        if (!dir) return;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            // Match "intel-rapl:N" (not sub-packages "intel-rapl:N:M")
            if (name.find("intel-rapl:") != 0) continue;
            std::string suffix = name.substr(11);
            if (suffix.find(':') != std::string::npos) continue;

            int cpu_idx;
            try { cpu_idx = std::stoi(suffix); }
            catch (...) { continue; }

            std::string cpu_path = base + "/" + name;
            std::string energy_file = cpu_path + "/energy_uj";

            struct stat st;
            if (stat(energy_file.c_str(), &st) != 0) continue;

            cpu_dirs_[cpu_idx] = cpu_path;

            // Read max_energy_range_uj for wraparound detection
            std::string max_range_file = cpu_path + "/max_energy_range_uj";
            std::ifstream mf(max_range_file);
            if (mf.is_open()) {
                unsigned long long max_uj = 0;
                mf >> max_uj;
                max_energy_j_[cpu_idx] = static_cast<double>(max_uj) / 1e6;
            }

            // Search for DRAM sub-package
            DIR* sub_dir = opendir(cpu_path.c_str());
            if (!sub_dir) continue;

            struct dirent* sub_entry;
            while ((sub_entry = readdir(sub_dir)) != nullptr) {
                std::string sub_name = sub_entry->d_name;
                if (sub_name.find("intel-rapl:") != 0) continue;
                if (sub_name.find(':', 11) == std::string::npos) continue;

                std::string sub_path = cpu_path + "/" + sub_name;
                std::string name_file = sub_path + "/name";
                std::ifstream nf(name_file);
                if (nf.is_open()) {
                    std::string pkg_name;
                    std::getline(nf, pkg_name);
                    while (!pkg_name.empty()
                           && (pkg_name.back() == '\n'
                               || pkg_name.back() == '\r'
                               || pkg_name.back() == ' '))
                        pkg_name.pop_back();
                    if (pkg_name == "dram") {
                        dram_dirs_[cpu_idx] = sub_path;
                        // Read DRAM max_energy_range_uj
                        std::string dram_max = sub_path + "/max_energy_range_uj";
                        std::ifstream dmf(dram_max);
                        if (dmf.is_open()) {
                            unsigned long long dmax = 0;
                            dmf >> dmax;
                            max_dram_energy_j_[cpu_idx] =
                                static_cast<double>(dmax) / 1e6;
                        }
                        break;
                    }
                }
            }
            closedir(sub_dir);
        }
        closedir(dir);
    }
#endif

    static double read_energy_uj(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) {
            throw std::runtime_error("Cannot read RAPL energy: " + path);
        }
        unsigned long long energy_uj = 0;
        f >> energy_uj;
        return static_cast<double>(energy_uj);
    }
};

} // namespace zeus
