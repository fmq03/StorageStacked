.PHONY: bootstrap build rtl memory link acceptance experiments paper
ACCEPTANCE_DIR ?= results/acceptance
bootstrap:
	bash env/bootstrap.sh
build:
	bash env/build.sh
rtl:
	python3 logic_die/tests/run.py --out $(ACCEPTANCE_DIR)/rtl
memory:
	mkdir -p $(ACCEPTANCE_DIR)
	ctest --test-dir build/system/mem_sim -R '^(sequence_tests|timing_boundary_tests|phy_tests|config_tests|model_config_tests|scheduler_contract_tests|refactor_tests|result_value_tests)$$' --output-on-failure --output-log $(abspath $(ACCEPTANCE_DIR))/native-memory.log
	python3 mem_sim/integration/check_online.py build/system/mem_sim/libstoragestacked_memsim.so $(ACCEPTANCE_DIR)/online-api
link:
	mkdir -p $(ACCEPTANCE_DIR)
	$(MAKE) -C axi2flit/systemc SYSTEMC_HOME=/usr preflight full-link-all full-link-negative > $(ACCEPTANCE_DIR)/link.log 2>&1
	tail -n 4 $(ACCEPTANCE_DIR)/link.log
acceptance:
	mkdir -p $(ACCEPTANCE_DIR)
	rm -f $(ACCEPTANCE_DIR)/summary.json
	$(MAKE) build
	$(MAKE) rtl memory link
	python3 scripts/experiments.py --quick --resume --out $(ACCEPTANCE_DIR)/system
	python3 scripts/negative_controls.py --rtl $(ACCEPTANCE_DIR)/rtl --system $(ACCEPTANCE_DIR)/system/c4-b2-logic_die --out $(ACCEPTANCE_DIR)/negative_controls.json
	python3 scripts/acceptance_summary.py $(ACCEPTANCE_DIR)
experiments:
	python3 scripts/experiments.py
paper:
	$(MAKE) -C paper
