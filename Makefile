# Makefile for libspaznet
# Provides convenient targets for building, testing, and code quality checks

.PHONY: all build clean test format check-tidy check-cppcheck check-format check lint help install
.PHONY: build-asan build-tsan build-msan build-ubsan build-debug
.PHONY: test-asan test-tsan test-msan test-ubsan test-valgrind
.PHONY: sanitizers help-sanitizers

# Default build directory
BUILD_DIR ?= build
CMAKE ?= cmake
MAKE ?= make
VALGRIND ?= valgrind

# Detect number of CPU cores for parallel builds
NPROC ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Sanitizer build directories
BUILD_ASAN ?= build-asan
BUILD_TSAN ?= build-tsan
BUILD_MSAN ?= build-msan
BUILD_UBSAN ?= build-ubsan
BUILD_DEBUG ?= build-debug

# Default target
all: build

# Help target
help:
	@echo "libspaznet Makefile"
	@echo ""
	@echo "Available targets:"
	@echo "  make              - Build the project (default)"
	@echo "  make build        - Build the project"
	@echo "  make clean        - Remove build directory"
	@echo "  make test         - Run all tests"
	@echo "  make test-unit    - Run unit tests only"
	@echo "  make test-integration - Run integration tests only"
	@echo "  make test-performance - Run performance tests only"
	@echo "  make format       - Format code with clang-format"
	@echo "  make check-format - Check code formatting"
	@echo "  make check-tidy   - Run clang-tidy static analysis"
	@echo "  make check-cppcheck - Run cppcheck static analysis"
	@echo "  make lint         - Run all code quality checks"
	@echo "  make install      - Install the library"
	@echo "  make help         - Show this help message"
	@echo ""
	@echo "Sanitizer targets:"
	@echo "  make build-asan   - Build with AddressSanitizer"
	@echo "  make build-tsan   - Build with ThreadSanitizer"
	@echo "  make build-msan   - Build with MemorySanitizer"
	@echo "  make build-ubsan - Build with UndefinedBehaviorSanitizer"
	@echo "  make build-debug - Build with debug symbols"
	@echo "  make test-asan   - Run tests with AddressSanitizer"
	@echo "  make test-tsan   - Run tests with ThreadSanitizer"
	@echo "  make test-msan   - Run tests with MemorySanitizer"
	@echo "  make test-ubsan  - Run tests with UndefinedBehaviorSanitizer"
	@echo "  make test-valgrind - Run tests with Valgrind"
	@echo "  make sanitizers  - Build and test with all sanitizers"
	@echo ""
	@echo "Environment variables:"
	@echo "  BUILD_DIR         - Build directory (default: build)"
	@echo "  CMAKE            - CMake executable (default: cmake)"
	@echo "  NPROC            - Number of parallel jobs (default: auto-detect)"
	@echo "  VALGRIND         - Valgrind executable (default: valgrind)"

# Configure CMake if needed
$(BUILD_DIR)/CMakeCache.txt:
	@echo "Configuring CMake..."
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && $(CMAKE) ..

# Build target
build: $(BUILD_DIR)/CMakeCache.txt
	@echo "Building with $(NPROC) parallel jobs..."
	@cd $(BUILD_DIR) && $(MAKE) -j$(NPROC)

# Clean target
clean:
	@echo "Cleaning build directories..."
	@rm -rf $(BUILD_DIR) $(BUILD_ASAN) $(BUILD_TSAN) $(BUILD_MSAN) $(BUILD_UBSAN) $(BUILD_DEBUG)
	@echo "Clean complete."

# Test targets
test: build
	@echo "Running all tests..."
	@cd $(BUILD_DIR) && ctest --output-on-failure

test-unit: build
	@echo "Running unit tests..."
	@cd $(BUILD_DIR) && ./test_unit

test-integration: build
	@echo "Running integration tests..."
	@cd $(BUILD_DIR) && ./test_integration

test-performance: build
	@echo "Running performance tests..."
	@cd $(BUILD_DIR) && ./test_performance

# Formatting targets
format:
	@echo "Formatting code with clang-format..."
	@find . -name "*.cpp" -o -name "*.hpp" | \
		grep -v "^\./build" | \
		grep -v "^\./\.git" | \
		xargs clang-format -i
	@echo "Formatting complete."

check-format:
	@echo "Checking code formatting..."
	@find . -name "*.cpp" -o -name "*.hpp" | \
		grep -v "^\./build" | \
		grep -v "^\./\.git" | \
		while read file; do \
			if ! clang-format --style=file --dry-run --Werror "$$file" > /dev/null 2>&1; then \
				echo "Formatting error in $$file"; \
				clang-format --style=file "$$file" | diff -u "$$file" - || true; \
				exit 1; \
			fi; \
		done
	@echo "All files are properly formatted."

# Clang-tidy target
check-tidy: build
	@echo "Running clang-tidy..."
	@which clang-tidy > /dev/null 2>&1 || (echo "clang-tidy not found. Install it to use this target." && exit 1)
	@cd $(BUILD_DIR) && \
		$(CMAKE) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON . && \
		run-clang-tidy -p . \
			-header-filter='include/.*' \
			-checks='-*,readability-*,performance-*,modernize-*,cppcoreguidelines-*' \
			-quiet \
			2>/dev/null || \
		clang-tidy -p . \
			-header-filter='include/.*' \
			-checks='-*,readability-*,performance-*,modernize-*,cppcoreguidelines-*' \
			$$(find ../src ../include -name "*.cpp" -o -name "*.hpp") \
			2>&1 | tee /tmp/clang-tidy-output.txt || true
	@echo "Clang-tidy analysis complete. See output above."

# Cppcheck target
check-cppcheck:
	@echo "Running cppcheck..."
	@which cppcheck > /dev/null 2>&1 || (echo "cppcheck not found. Install it to use this target." && exit 1)
	@cppcheck \
		--enable=all \
		--suppress=missingIncludeSystem \
		--suppress=unusedFunction \
		--suppress=unmatchedSuppression \
		--inline-suppr \
		--std=c++20 \
		--project=$(BUILD_DIR)/compile_commands.json \
		--output-file=$(BUILD_DIR)/cppcheck-report.txt \
		--xml --xml-version=2 \
		2>&1 | tee $(BUILD_DIR)/cppcheck-output.txt || true
	@if [ -f $(BUILD_DIR)/cppcheck-report.txt ]; then \
		echo "Cppcheck report saved to $(BUILD_DIR)/cppcheck-report.txt"; \
	fi
	@echo "Cppcheck analysis complete."

# Combined lint target
lint: check-format check-tidy check-cppcheck
	@echo "All code quality checks complete."

# Install target
install: build
	@echo "Installing library..."
	@cd $(BUILD_DIR) && $(MAKE) install

# Development workflow targets
dev-setup: build
	@echo "Setting up development environment..."
	@cd $(BUILD_DIR) && $(CMAKE) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON .
	@if [ ! -f compile_commands.json ]; then \
		ln -s $(BUILD_DIR)/compile_commands.json .; \
		echo "Created symlink to compile_commands.json"; \
	fi

# Quick check (format + build + test)
quick-check: check-format build test
	@echo "Quick check complete."

# Full CI check (all checks + tests)
ci: clean format check-format build test check-tidy check-cppcheck
	@echo "CI checks complete."

# Coverage target (requires gcov/lcov)
coverage: build
	@echo "Generating coverage report..."
	@which lcov > /dev/null 2>&1 || (echo "lcov not found. Install it to use this target." && exit 1)
	@cd $(BUILD_DIR) && \
		$(CMAKE) -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON . && \
		$(MAKE) -j$(NPROC) && \
		ctest && \
		lcov --capture --directory . --output-file coverage.info && \
		lcov --remove coverage.info '/usr/*' --output-file coverage.info && \
		genhtml coverage.info --output-directory coverage-html
	@echo "Coverage report generated in $(BUILD_DIR)/coverage-html/"

# Documentation target (requires Doxygen)
docs:
	@echo "Generating documentation..."
	@which doxygen > /dev/null 2>&1 || (echo "doxygen not found. Install it to use this target." && exit 1)
	@doxygen Doxyfile 2>/dev/null || echo "Create a Doxyfile to generate documentation"

# Example target
example: build
	@echo "Running example..."
	@cd $(BUILD_DIR) && ./example

# Show build configuration
show-config:
	@echo "Build Configuration:"
	@echo "  Build directory: $(BUILD_DIR)"
	@echo "  CMake: $(CMAKE)"
	@echo "  Make: $(MAKE)"
	@echo "  Parallel jobs: $(NPROC)"
	@if [ -f $(BUILD_DIR)/CMakeCache.txt ]; then \
		echo "  CMake configured: Yes"; \
		grep CMAKE_BUILD_TYPE $(BUILD_DIR)/CMakeCache.txt 2>/dev/null | head -1 || true; \
	else \
		echo "  CMake configured: No"; \
	fi

# Sanitizer build targets
build-asan: $(BUILD_ASAN)/CMakeCache.txt
	@echo "Building with AddressSanitizer..."
	@cd $(BUILD_ASAN) && $(MAKE) -j$(NPROC)

build-tsan: $(BUILD_TSAN)/CMakeCache.txt
	@echo "Building with ThreadSanitizer..."
	@cd $(BUILD_TSAN) && $(MAKE) -j$(NPROC)

build-msan: $(BUILD_MSAN)/CMakeCache.txt
	@echo "Building with MemorySanitizer..."
	@cd $(BUILD_MSAN) && $(MAKE) -j$(NPROC)

build-ubsan: $(BUILD_UBSAN)/CMakeCache.txt
	@echo "Building with UndefinedBehaviorSanitizer..."
	@cd $(BUILD_UBSAN) && $(MAKE) -j$(NPROC)

build-debug: $(BUILD_DEBUG)/CMakeCache.txt
	@echo "Building with debug symbols..."
	@cd $(BUILD_DEBUG) && $(MAKE) -j$(NPROC)

# Sanitizer CMake configuration
$(BUILD_ASAN)/CMakeCache.txt:
	@echo "Configuring CMake with AddressSanitizer..."
	@mkdir -p $(BUILD_ASAN)
	@cd $(BUILD_ASAN) && $(CMAKE) .. \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
		-DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
		-DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address"

$(BUILD_TSAN)/CMakeCache.txt:
	@echo "Configuring CMake with ThreadSanitizer..."
	@mkdir -p $(BUILD_TSAN)
	@cd $(BUILD_TSAN) && $(CMAKE) .. \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
		-DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
		-DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"

$(BUILD_MSAN)/CMakeCache.txt:
	@echo "Configuring CMake with MemorySanitizer..."
	@mkdir -p $(BUILD_MSAN)
	@cd $(BUILD_MSAN) && $(CMAKE) .. \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_CXX_FLAGS="-fsanitize=memory -fno-omit-frame-pointer -g -fno-optimize-sibling-calls" \
		-DCMAKE_C_FLAGS="-fsanitize=memory -fno-omit-frame-pointer -g -fno-optimize-sibling-calls" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=memory" \
		-DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=memory"

$(BUILD_UBSAN)/CMakeCache.txt:
	@echo "Configuring CMake with UndefinedBehaviorSanitizer..."
	@mkdir -p $(BUILD_UBSAN)
	@cd $(BUILD_UBSAN) && $(CMAKE) .. \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer -g" \
		-DCMAKE_C_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer -g" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=undefined" \
		-DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=undefined"

$(BUILD_DEBUG)/CMakeCache.txt:
	@echo "Configuring CMake with debug symbols..."
	@mkdir -p $(BUILD_DEBUG)
	@cd $(BUILD_DEBUG) && $(CMAKE) .. \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_CXX_FLAGS="-g -O0" \
		-DCMAKE_C_FLAGS="-g -O0"

# Sanitizer test targets
test-asan: build-asan
	@echo "Running tests with AddressSanitizer..."
	@cd $(BUILD_ASAN) && \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:abort_on_error=1 \
		ctest --output-on-failure

test-tsan: build-tsan
	@echo "Running tests with ThreadSanitizer..."
	@cd $(BUILD_TSAN) && \
		TSAN_OPTIONS=halt_on_error=1:abort_on_error=1 \
		ctest --output-on-failure

test-msan: build-msan
	@echo "Running tests with MemorySanitizer..."
	@echo "Note: MemorySanitizer requires instrumented libraries"
	@cd $(BUILD_MSAN) && \
		MSAN_OPTIONS=halt_on_error=1:abort_on_error=1 \
		ctest --output-on-failure

test-ubsan: build-ubsan
	@echo "Running tests with UndefinedBehaviorSanitizer..."
	@cd $(BUILD_UBSAN) && \
		UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_stacktrace=1 \
		ctest --output-on-failure

test-valgrind: build-debug
	@echo "Running tests with Valgrind..."
	@VALGRIND_CMD=$$(which valgrind 2>/dev/null || echo "$(VALGRIND)"); \
	if [ -z "$$VALGRIND_CMD" ] || ! command -v $$VALGRIND_CMD > /dev/null 2>&1; then \
		echo "valgrind not found. Install it to use this target."; \
		exit 1; \
	fi; \
	cd $(BUILD_DEBUG) && \
		$$VALGRIND_CMD --leak-check=full \
			--show-leak-kinds=all \
			--track-origins=yes \
			--error-exitcode=1 \
			--suppressions=../valgrind.supp \
			--log-file=valgrind-$$(date +%Y%m%d-%H%M%S).log \
			ctest --output-on-failure || true
	@echo "Valgrind analysis complete. Check output above and log files for issues."

# Combined sanitizer target
sanitizers: build-asan build-tsan build-ubsan
	@echo "Running all sanitizer tests..."
	@echo ""
	@echo "=== AddressSanitizer ==="
	@$(MAKE) test-asan || true
	@echo ""
	@echo "=== ThreadSanitizer ==="
	@$(MAKE) test-tsan || true
	@echo ""
	@echo "=== UndefinedBehaviorSanitizer ==="
	@$(MAKE) test-ubsan || true
	@echo ""
	@echo "All sanitizer tests complete."

# Help for sanitizers
help-sanitizers:
	@echo "Sanitizer Build Targets:"
	@echo ""
	@echo "  make build-asan   - Build with AddressSanitizer (detects memory errors)"
	@echo "  make build-tsan   - Build with ThreadSanitizer (detects data races)"
	@echo "  make build-msan   - Build with MemorySanitizer (detects uninitialized memory)"
	@echo "  make build-ubsan  - Build with UndefinedBehaviorSanitizer (detects UB)"
	@echo "  make build-debug  - Build with debug symbols (for valgrind/gdb)"
	@echo ""
	@echo "Sanitizer Test Targets:"
	@echo ""
	@echo "  make test-asan    - Run tests with AddressSanitizer"
	@echo "  make test-tsan    - Run tests with ThreadSanitizer"
	@echo "  make test-msan    - Run tests with MemorySanitizer"
	@echo "  make test-ubsan   - Run tests with UndefinedBehaviorSanitizer"
	@echo "  make test-valgrind - Run tests with Valgrind"
	@echo "  make sanitizers   - Build and test with all sanitizers"
	@echo ""
	@echo "Notes:"
	@echo "  - Sanitizers require a debug build"
	@echo "  - ThreadSanitizer and AddressSanitizer cannot be used together"
	@echo "  - MemorySanitizer requires instrumented system libraries"
	@echo "  - Valgrind requires debug symbols (use build-debug)"

