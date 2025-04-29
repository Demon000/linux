$(src)/gmsl/%.dtsi: $(src)/gmsl/%.json
	$(src)/gmsl/gen_gmsl_dts.py $< -o $@

$(src)/gmsl/%.dtso: $(src)/gmsl/%.json
	$(src)/gmsl/gen_gmsl_dts.py --dtbo $< -o $@
