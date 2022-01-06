hl:
	if ! [ -d ./gem5/configs/csh ]; then \
		mkdir -v ./gem5/configs/csh; \
	fi
	if ! [ -d ./gem5/src/csh ]; then \
		mkdir -v ./gem5/src/csh; \
	fi
	ln -f ./configs/csh/* ./gem5/configs/csh
	ln -f ./src/csh/* ./gem5/src/csh

build:
	make hl
	scons -C ./gem5 ./gem5/build/RISCV/gem5.opt -j 12

csh:
	./gem5/build/RISCV/gem5.opt \
		--debug-flags=SecCtrl \
		./gem5/configs/csh/config.py \
			--mem-size=10890809856B \
			--caches \
			--cpu-type=DerivO3CPU \
			--l1d_size=64kB \
			--l1i_size=16kB \
			--cmd=./gem5/tests/test-progs/hello/bin/riscv/linux/hello \
			--nvm-type=NVM_2400_1x64 \
			--nvm-ranks=1

se:
	./gem5/build/RISCV/gem5.opt \
		./gem5/configs/example/se.py \
			--mem-size=8589934592B \
			--caches \
			--cpu-type=DerivO3CPU \
			--l1d_size=64kB \
			--l1i_size=16kB \
			--cmd=./gem5/tests/test-progs/hello/bin/riscv/linux/hello
