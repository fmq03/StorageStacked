.PHONY: bootstrap build rtl memory link acceptance experiments paper
bootstrap:
	bash env/bootstrap.sh
build:
	bash env/build.sh
rtl:
	python3 logic_die/tests/run.py --out results/acceptance/rtl
memory:
	ctest --test-dir build/system/mem_sim -R '^(sequence_tests|timing_boundary_tests|phy_tests|config_tests|model_config_tests|scheduler_contract_tests|refactor_tests|result_value_tests)$$' --output-on-failure
	python3 mem_sim/integration/check_online.py build/system/mem_sim/libstoragestacked_memsim.so results/acceptance/online-api
link:
	$(MAKE) -C axi2flit/systemc SYSTEMC_HOME=/usr preflight full-link-all full-link-negative
acceptance: build
	$(MAKE) rtl memory link
	python3 scripts/experiments.py --quick --out results/acceptance/system
	python3 scripts/negative_controls.py
experiments:
	python3 scripts/experiments.py
paper:
	$(MAKE) -C paper
