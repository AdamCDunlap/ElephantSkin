#CXXFLAGS = -std=c++14 -Wall -Wextra -stdlib=libc++
CXXFLAGS = -std=c++14 -Wall -Wextra -g

ElephantSkin: ElephantSkin.cc
	clang++ $(CXXFLAGS) `pkg-config fuse --cflags --libs` $< -o $@
