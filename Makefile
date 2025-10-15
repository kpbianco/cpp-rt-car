CHARACTERIZE_OUT ?= reports/characterize/$(shell date +%Y%m%d_%H%M%S)
CHARACTERIZE_BINARY ?= ./build/bin/rtfw_demo
CHARACTERIZE_DURATION ?= 10s
CHARACTERIZE_THREADS ?= auto
CHARACTERIZE_SMT ?= auto
CHARACTERIZE_PROFILE ?=

ifneq ($(strip $(CHARACTERIZE_PROFILE)),)
CHARACTERIZE_PROFILE_ARG := --profile $(CHARACTERIZE_PROFILE)
else
CHARACTERIZE_PROFILE_ARG :=
endif

.PHONY: characterize
characterize:
	@mkdir -p $(CHARACTERIZE_OUT)
	@echo "Running characterization into $(CHARACTERIZE_OUT)"
	@python3 tools/characterize/run_all.py \
		--binary $(CHARACTERIZE_BINARY) \
		--out-dir $(CHARACTERIZE_OUT) \
		--duration $(CHARACTERIZE_DURATION) \
		--threads $(CHARACTERIZE_THREADS) \
		--smt $(CHARACTERIZE_SMT) \
		$(CHARACTERIZE_PROFILE_ARG)
	@echo "Artifacts available under $(CHARACTERIZE_OUT)"
