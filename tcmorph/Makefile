# tcmorph
#
#   make            examples をビルド (bin/ に出力)
#   make check      依存の確認
#   make install    ヘッダを $(PREFIX)/include にコピー (既定 /usr/local)
#   make clean

CXX      ?= g++
PREFIX   ?= /usr/local
EIGEN    ?= /usr/include/eigen3

CXXSTD   := -std=c++17
OPT      := -O3 -march=native
OMP      := -fopenmp
INCLUDES := -Iinclude -I$(EIGEN)
CXXFLAGS ?= $(CXXSTD) $(OPT) $(OMP) $(INCLUDES)

HEADERS := $(wildcard include/tcmorph/*.hpp)
EXAMPLES := bin/morph_aligner bin/morph_generalized

.PHONY: all check install uninstall clean

all: $(EXAMPLES)

bin:
	@mkdir -p bin

# morphingAligner / morphingSoundGenerator 系 (edit.mat)
bin/morph_aligner: examples/morph_aligner.cpp $(HEADERS) | bin
	$(CXX) $(CXXFLAGS) $< -o $@

# morphContinuumGen / threeItemMorpherGeneralUI 系 (morphdBase)
bin/morph_generalized: examples/morph_generalized.cpp $(HEADERS) | bin
	$(CXX) $(CXXFLAGS) $< -o $@

check:
	@test -d $(EIGEN) \
	  && echo "Eigen 3.4       : OK ($(EIGEN))" \
	  || echo "Eigen 3.4       : 見つかりません。make EIGEN=<path> か apt install libeigen3-dev"
	@test -f /usr/include/nlohmann/json.hpp \
	  && echo "nlohmann/json   : OK" \
	  || echo "nlohmann/json   : 見つかりません。apt install nlohmann-json3-dev"
	@printf '#include <omp.h>\nint main(){return omp_get_max_threads()>0?0:1;}\n' \
	  | $(CXX) $(OMP) -x c++ - -o /tmp/tcmorph_omp_check 2>/dev/null \
	  && echo "OpenMP          : OK ($$(/tmp/tcmorph_omp_check && echo 有効))" \
	  || echo "OpenMP          : 使えません。CXXFLAGS から -fopenmp を外してください"
	@python3 -c "import numpy, scipy, pyworld, soundfile, h5py" 2>/dev/null \
	  && echo "tools の依存    : OK" \
	  || echo "tools の依存    : 不足。pip install numpy scipy h5py pyworld soundfile"

install: $(HEADERS)
	@mkdir -p $(PREFIX)/include/tcmorph
	cp $(HEADERS) $(PREFIX)/include/tcmorph/
	@echo "インストール先: $(PREFIX)/include/tcmorph"

uninstall:
	rm -rf $(PREFIX)/include/tcmorph

clean:
	rm -rf bin
