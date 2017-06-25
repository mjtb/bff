#ifndef BFF_H_INCLUDED
#define BFF_H_INCLUDED

#include <string>

class cliopts
{

public:

	std::wstring input;
	std::wstring output;
	int help;

	cliopts(int argc, wchar_t ** argv);
	int check_syntax();
	void print_syntax_help();

};

extern int bff(const cliopts & opts);
extern std::string utf8(const std::wstring & s);
extern std::wstring utf8(const std::string & s);
extern std::string ansi(const std::wstring & s);
extern std::wstring ansi(const std::string & s);


#endif
