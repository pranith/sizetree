CMAKE ?= cmake
CTEST ?= ctest
BUILD_DIR ?= build
STATIC_BUILD_DIR ?= build-static
CMAKE_ARGS ?=
STATIC_CXX ?=

.PHONY: all static test clean

all:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release $(CMAKE_ARGS) -DSIZETREE_STATIC=OFF
	+$(CMAKE) --build "$(BUILD_DIR)"

static:
	$(CMAKE) -S . -B "$(STATIC_BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release $(CMAKE_ARGS) -DSIZETREE_STATIC=ON $(if $(strip $(STATIC_CXX)),-DCMAKE_CXX_COMPILER="$(STATIC_CXX)")
	+$(CMAKE) --build "$(STATIC_BUILD_DIR)"

test: all
	$(CTEST) --test-dir "$(BUILD_DIR)" --output-on-failure

clean:
	@clean_status=0; \
	$(RM) -r -- "$(BUILD_DIR)" "$(STATIC_BUILD_DIR)" || clean_status=$$?; \
	if [ "$$clean_status" -ne 0 ]; then \
		nfs_files=$$(find "$(BUILD_DIR)" "$(STATIC_BUILD_DIR)" -type f -name '.nfs*' -print 2>/dev/null); \
		if [ -n "$$nfs_files" ]; then \
			printf '%s\n' \
				'clean: Remaining NFS files may still be held by a running or suspended process:' \
				"$$nfs_files" \
				'Exit any sizetree instances using these builds (fg then q if suspended), then retry make clean.' \
				'Use lsof -- FILE or fuser -v FILE on the host running the process to identify it.' >&2; \
		fi; \
	fi; \
	exit "$$clean_status"
