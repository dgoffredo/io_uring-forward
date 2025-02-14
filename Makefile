echo-server: echo-server.cpp echo-server.cflags echo-server.lflags
	$(CXX) $(file < echo-server.cflags) -o $@ $< $(file < echo-server.lflags)

.PHONY: format
format:
	clang-format -i --style='{BasedOnStyle: Google, Language: Cpp, ColumnLimit: 80}' *.cpp
