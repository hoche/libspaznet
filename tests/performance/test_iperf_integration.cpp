#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#define popen _popen
#define pclose _pclose
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

class IperfIntegrationTest : public ::testing::Test {
  protected:
    std::string find_iperf_executable() {
        // Try iperf (iperf2) first, then iperf3
        const char* candidates[] = {"iperf", "iperf3"};

        for (const char* cmd : candidates) {
            std::string test_cmd = std::string(cmd) + " --version > /dev/null 2>&1";
            if (system(test_cmd.c_str()) == 0) {
                return std::string(cmd);
            }
        }

        return "";
    }

    bool is_iperf3(const std::string& iperf) {
        return iperf.find("iperf3") != std::string::npos;
    }

    void SetUp() override {
        iperf_executable_ = find_iperf_executable();
        if (iperf_executable_.empty()) {
            GTEST_SKIP() << "iperf/iperf3 not found. Install iperf3 for bandwidth testing.";
        }

        server_port_ = 5201;
        server_process_id_ = 0;
        server_running_ = false;
    }

    void TearDown() override {
        stop_iperf_server();
    }

    bool start_iperf_server(int port, int duration_seconds = 300, bool udp = false) {
        if (iperf_executable_.empty()) {
            return false;
        }

        stop_iperf_server(); // Stop any existing server

        std::ostringstream cmd;
        if (is_iperf3(iperf_executable_)) {
            // iperf3 syntax
            cmd << iperf_executable_ << " -s -p " << port;
        } else {
            // iperf2 syntax (uses -t for time, but server doesn't need it)
            cmd << iperf_executable_ << " -s -p " << port;
            // For iperf2, UDP requires the server to be started with `-u`.
            if (udp) {
                cmd << " -u";
            }
        }

        std::cout << "\n[IPERF] Starting server: " << cmd.str() << std::endl;

#ifdef _WIN32
        STARTUPINFOA si = {sizeof(si)};
        PROCESS_INFORMATION pi;
        std::string cmd_str = cmd.str();
        char* cmd_cstr = new char[cmd_str.length() + 1];
        std::strcpy(cmd_cstr, cmd_str.c_str());

        if (CreateProcessA(nullptr, cmd_cstr, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &si, &pi)) {
            server_process_id_ = pi.dwProcessId;
            server_handle_ = pi.hProcess;
            CloseHandle(pi.hThread);
            server_running_ = true;
            delete[] cmd_cstr;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            return true;
        }
        delete[] cmd_cstr;
        return false;
#else
        pid_t pid = fork();
        if (pid == 0) {
            // Child process - redirect output to /dev/null
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            execlp("/bin/sh", "sh", "-c", cmd.str().c_str(), nullptr);
            _exit(1); // Use _exit in child to avoid flushing buffers
        } else if (pid > 0) {
            // Parent process
            server_process_id_ = pid;
            server_running_ = true;
            // Wait a moment for server to start
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            // Check if process is still running
            if (waitpid(pid, nullptr, WNOHANG) == 0) {
                return true;
            }
            // Process exited immediately
            server_running_ = false;
            server_process_id_ = 0;
            return false;
        }
        return false;
#endif
    }

    void stop_iperf_server() {
        if (!server_running_ || server_process_id_ == 0) {
            return;
        }

        std::cout << "\n[IPERF] Stopping server (PID: " << server_process_id_ << ")" << std::endl;

#ifdef _WIN32
        if (server_handle_) {
            TerminateProcess(server_handle_, 0);
            CloseHandle(server_handle_);
            server_handle_ = nullptr;
        }
#else
        kill(server_process_id_, SIGTERM);
        // Wait for process to terminate
        int status;
        for (int i = 0; i < 10; ++i) {
            if (waitpid(server_process_id_, &status, WNOHANG) == server_process_id_) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // Force kill if still running
        kill(server_process_id_, SIGKILL);
        waitpid(server_process_id_, nullptr, 0);
#endif

        server_running_ = false;
        server_process_id_ = 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::string run_iperf_client(const std::string& host, int port, int duration_seconds,
                                 bool udp = false) {
        if (iperf_executable_.empty()) {
            return "";
        }

        std::ostringstream cmd;
        if (is_iperf3(iperf_executable_)) {
            // iperf3 syntax
            cmd << iperf_executable_ << " -c " << host << " -p " << port << " -t "
                << duration_seconds;
            if (udp) {
                cmd << " -u -b 100M";
            }
        } else {
            // iperf2 syntax
            cmd << iperf_executable_ << " -c " << host << " -p " << port << " -t "
                << duration_seconds;
            if (udp) {
                cmd << " -u -b 100M";
            }
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

    std::string iperf_executable_;
    int server_port_;
#ifdef _WIN32
    DWORD server_process_id_;
    HANDLE server_handle_{nullptr};
#else
    pid_t server_process_id_;
#endif
    bool server_running_;
};

TEST_F(IperfIntegrationTest, IperfAvailable) {
    std::cout << "\n[IPERF] Found: " << iperf_executable_ << std::endl;
    EXPECT_FALSE(iperf_executable_.empty());
}

TEST_F(IperfIntegrationTest, TCPBandwidthTest) {
    const int duration = 3;

    // Automatically start iperf server
    ASSERT_TRUE(start_iperf_server(server_port_, /*duration_seconds=*/300, /*udp=*/false))
        << "Failed to start iperf server";

    std::cout << "\n[IPERF] TCP Bandwidth Test:" << std::endl;
    std::cout << "  Server: " << iperf_executable_ << " on port " << server_port_ << std::endl;

    std::string result = run_iperf_client("127.0.0.1", server_port_, duration, false);

    if (!result.empty()) {
        std::cout << "  Result: " << result << std::endl;
        EXPECT_NE(result.find("connected"), std::string::npos);
    } else {
        FAIL() << "iperf client failed to connect or produce output";
    }
}

TEST_F(IperfIntegrationTest, UDPBandwidthTest) {
    const int duration = 3;

    // Automatically start iperf server
    ASSERT_TRUE(start_iperf_server(server_port_, /*duration_seconds=*/300, /*udp=*/true))
        << "Failed to start iperf server";

    std::cout << "\n[IPERF] UDP Bandwidth Test:" << std::endl;
    std::cout << "  Server: " << iperf_executable_ << " on port " << server_port_ << std::endl;

    std::string result = run_iperf_client("127.0.0.1", server_port_, duration, true);

    if (!result.empty()) {
        std::cout << "  Result: " << result << std::endl;
        EXPECT_NE(result.find("connected"), std::string::npos);
    } else {
        FAIL() << "iperf client failed to connect or produce output";
    }
}
