#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#define popen _popen
#define pclose _pclose
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

class IperfIntegrationTest : public ::testing::Test {
protected:
    std::string find_iperf_executable() {
        // Try iperf3 first, then iperf
        const char* candidates[] = {"iperf3", "iperf"};
        
        for (const char* cmd : candidates) {
            std::string test_cmd = std::string(cmd) + " --version > /dev/null 2>&1";
            if (system(test_cmd.c_str()) == 0) {
                return std::string(cmd);
            }
        }
        
        return "";
    }
    
    bool run_iperf_server(int port, int duration_seconds) {
        std::string iperf = find_iperf_executable();
        if (iperf.empty()) {
            return false;
        }
        
        std::ostringstream cmd;
        cmd << iperf << " -s -p " << port << " -t " << duration_seconds;
        
        std::cout << "\n[IPERF] Starting server: " << cmd.str() << std::endl;
        
#ifdef _WIN32
        STARTUPINFOA si = {sizeof(si)};
        PROCESS_INFORMATION pi;
        if (!CreateProcessA(nullptr, const_cast<char*>(cmd.str().c_str()),
                          nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
#else
        int pid = fork();
        if (pid == 0) {
            // Child process
            execlp("/bin/sh", "sh", "-c", cmd.str().c_str(), nullptr);
            exit(1);
        } else if (pid > 0) {
            // Parent process
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            return true;
        }
        return false;
#endif
    }
    
    std::string run_iperf_client(const std::string& host, int port, int duration_seconds, bool udp = false) {
        std::string iperf = find_iperf_executable();
        if (iperf.empty()) {
            return "";
        }
        
        std::ostringstream cmd;
        cmd << iperf << " -c " << host << " -p " << port << " -t " << duration_seconds;
        if (udp) {
            cmd << " -u";
        }
        
#ifdef _WIN32
        cmd << " 2>&1";
        FILE* pipe = _popen(cmd.str().c_str(), "r");
#else
        cmd << " 2>&1";
        FILE* pipe = popen(cmd.str().c_str(), "r");
#endif
        
        if (!pipe) {
            return "";
        }
        
        std::string result;
        char buffer[128];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }
        
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        
        return result;
    }
};

TEST_F(IperfIntegrationTest, IperfAvailable) {
    std::string iperf = find_iperf_executable();
    
    if (iperf.empty()) {
        GTEST_SKIP() << "iperf/iperf3 not found. Install iperf3 for bandwidth testing.";
    }
    
    std::cout << "\n[IPERF] Found: " << iperf << std::endl;
    EXPECT_FALSE(iperf.empty());
}

TEST_F(IperfIntegrationTest, TCPBandwidthTest) {
    std::string iperf = find_iperf_executable();
    if (iperf.empty()) {
        GTEST_SKIP() << "iperf/iperf3 not found";
    }
    
    const int port = 5201;
    const int duration = 3;
    
    // Note: This test requires iperf server to be running separately
    // or we could start it in a separate thread/process
    
    std::cout << "\n[IPERF] TCP Bandwidth Test:" << std::endl;
    std::cout << "  Note: This test requires an iperf server running on port " << port << std::endl;
    std::cout << "  Run: iperf3 -s -p " << port << std::endl;
    
    // For now, just verify we can run the command
    std::string result = run_iperf_client("127.0.0.1", port, duration, false);
    
    if (!result.empty()) {
        std::cout << "  Result: " << result << std::endl;
    }
}

TEST_F(IperfIntegrationTest, UDPBandwidthTest) {
    std::string iperf = find_iperf_executable();
    if (iperf.empty()) {
        GTEST_SKIP() << "iperf/iperf3 not found";
    }
    
    const int port = 5201;
    const int duration = 3;
    
    std::cout << "\n[IPERF] UDP Bandwidth Test:" << std::endl;
    std::cout << "  Note: This test requires an iperf server running on port " << port << std::endl;
    
    std::string result = run_iperf_client("127.0.0.1", port, duration, true);
    
    if (!result.empty()) {
        std::cout << "  Result: " << result << std::endl;
    }
}

