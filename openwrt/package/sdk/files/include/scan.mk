include $(TOPDIR)/include/verbose.mk

SCAN_TARGET ?= packageinfo
SCAN_NAME ?= package
SCAN_DIR ?= package
SCAN_DEPS ?= include/package.mk

ifeq ($(IS_TTY),1)
  define progress
	printf "\033[M\r$(1)" >&2;
  endef
else
  define progress
	:
  endef
endif

SCAN = $(patsubst $(SCAN_DIR)/%/Makefile,%,$(wildcard $(SCAN_DIR)/*/Makefile))
tmp/.$(SCAN_TARGET):
	@($(call progress,Collecting $(SCAN_NAME) info: merging...))
	for file in $(SCAN); do \
		cat tmp/info/.$(SCAN_TARGET)-$$file; \
	done > $@
	@($(call progress,Collecting $(SCAN_NAME) info: done))
	@echo

ifneq ($(SCAN_EXTRA),)
SCAN_STAMP=tmp/info/.scan-$(SCAN_TARGET)-$(shell ls $(SCAN_EXTRA) 2>/dev/null | (md5sum || md5) 2>/dev/null | cut -d' ' -f1)
$(SCAN_STAMP):
	rm -f tmp/info/.scan-$(SCAN_TARGET)-*
	touch $@
endif

# FIXME: generate this dynamically?
ifeq ($(SCAN_TARGET),packageinfo)
tmp/info/.packageinfo-kernel: $(wildcard package/kernel/modules/*.mk)
endif

define scanfiles
$(foreach FILE,$(SCAN),
  tmp/.$(SCAN_TARGET): tmp/info/.$(SCAN_TARGET)-$(FILE) $(SCAN_TARGET_DEPS) $(SCAN_DEPS)
  tmp/info/.$(SCAN_TARGET)-$(FILE): $(SCAN_DIR)/$(FILE)/Makefile $(SCAN_STAMP) $(SCAN_TARGET_DEPS)
	grep -E 'include (\$$$$\(INCLUDE_DIR\)|\$$$$\(TOPDIR\)/include)/' $(SCAN_DIR)/$(FILE)/Makefile >/dev/null && { \
		$$(call progress,Collecting $(SCAN_NAME) info: $(SCAN_DIR)/$(FILE)); \
		echo Source-Makefile: $(SCAN_DIR)/$(FILE)/Makefile; \
		$(NO_TRACE_MAKE) --no-print-dir DUMP=1 -C $(SCAN_DIR)/$(FILE) 3>/dev/null || echo "ERROR: please fix $(SCAN_DIR)/$(FILE)/Makefile" >&2; \
		echo; \
	} | tee $$@ || true
)

endef
$(eval $(call scanfiles))

FORCE:
.PHONY: FORCE
