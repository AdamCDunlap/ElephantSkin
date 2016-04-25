CXXFLAGS = -std=c++14 -Wall -Wextra

ElephantSkin: ElephantSkin.cc
	clang++ $(CXXFLAGS) `pkg-config fuse --cflags --libs` $< -o $@
