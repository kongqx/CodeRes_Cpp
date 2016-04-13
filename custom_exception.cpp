#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <climits>
#include <memory>
#include <exception>

/*
 * 构造函数不要忘记 explicit
 * 注意 what() 的重载方法
 * 线程中的异常不会被main捕捉，要自己捕捉处理。TODO demo
 */

struct InvalidInputException : std::exception {
    explicit InvalidInputException( const std::string &what )
            : whatString(what) {}

    explicit InvalidInputException( const std::string &inputStr,
                                    const std::string &desc )
            : whatString("Input string \"")
    { whatString.append( inputStr ).append( "\" is not valid! " ).append(desc); }

    virtual const char* what() const throw()
    { return whatString.c_str(); }

    std::string     whatString;
};


static
void test()
{
    int i = 10;
    //!! 临时的stringstream
    throw InvalidInputException( (std::stringstream() 
                << "Invaild arg: " << i).str() );
}


int main( int argc, char **argv )
{
    using namespace std;

    try {
        test();
        
    } catch (const std::exception &ex) {
        cerr << ex.what() << endl;
    } // try
    

    return 0;
}
