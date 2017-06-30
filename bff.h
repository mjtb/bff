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


class ffmpeg_error : public std::runtime_error
{
private:
	int _er;
	std::string _fn;
	std::string _arg;
	std::string _msg;
	static std::string format_message(int er, const char * fn, const char * arg);
public:
	explicit ffmpeg_error(int av_error_code, const char * function_name, const char * function_args) : _er(av_error_code), _fn(function_name), _arg(function_args), _msg(format_message(av_error_code, function_name, function_args)), std::runtime_error(_msg)
	{}
	ffmpeg_error(const ffmpeg_error &e) : _er(e._er), _fn(e._fn), _arg(e._arg), _msg(e._msg), std::runtime_error(_msg)
	{}
	~ffmpeg_error()
	{}
	int error_code() const
	{
		return _er;
	}
	const char * function_name() const
	{
		return _fn.c_str();
	}
	const char * function_args() const
	{
		return _arg.c_str();
	}
};

extern int bff(const cliopts & opts);
extern std::string utf8(const std::wstring & s);
extern std::wstring utf8(const std::string & s);
extern std::string ansi(const std::wstring & s);
extern std::wstring ansi(const std::string & s);


#endif
