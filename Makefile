echo-server: echo-server.cpp echo-server.flags
	$(CXX) $(file < echo-server.flags) -o $@ $<
