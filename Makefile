# Thin wrapper — all real logic lives in src/Makefile.
# Usage from project root is unchanged: make iso, make run, etc.
%:
	@$(MAKE) -C src $@

.DEFAULT_GOAL := all
