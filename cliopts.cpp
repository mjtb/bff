#include "stdafx.h"

#include "bff.h"


#include "getopt.h"

cliopts::cliopts(int argc, wchar_t ** argv) : help(0)
{
	int c;
	static struct option long_options[] = {
		{ L"input", 1, nullptr, 'i' },
		{ L"in", 1, nullptr, 'i' },
		{ L"output", 1, nullptr, 'o' },
		{ L"out", 1, nullptr, 'o' },
		{ L"help", 0, nullptr, 'h' },
		{ nullptr, 0, nullptr, 0 }
	};
	int option_index = 0;
	while ((c = getopt_long(argc, argv, L"i:o:h?", long_options, &option_index)) != -1) {
		switch (c) {
		case 'i':
			input = optarg;
			break;
		case 'o':
			output = optarg;
			break;
		case 'h':
		case '?':
			help = true;
			break;
		}
	}
}

int cliopts::check_syntax()
{
	if (help) {
		return 1;
	} else if (input.empty()) {
		std::cerr << "error: missing required argument: --input" << std::endl;
		return 2;
	} else if (output.empty()) {
		std::cerr << "error: missing required argument: --output" << std::endl;
		return 2;
	}
	return 0;
}

void cliopts::print_syntax_help()
{
	std::cout << "syntax: bff --input infile --output outfile options..." << std::endl;
}