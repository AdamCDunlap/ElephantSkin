CXXFLAGS = -std=c++14 -Wall -Wextra -stdlib=libc++

ElephantSkin: ElephantSkin.cc
	clang++ $(CXXFLAGS) `pkg-config fuse --cflags --libs` $< -o $@
