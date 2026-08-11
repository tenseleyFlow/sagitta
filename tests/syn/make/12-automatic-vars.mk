out: in
	$(info build) printf '%s' $@ $< $^ $? $* $+ $% $A | tee log
