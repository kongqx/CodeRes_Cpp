/* The following code example is taken from the book
 * "C++ Templates - The Complete Guide"
 * by David Vandevoorde and Nicolai M. Josuttis, Addison-Wesley, 2002
 *
 * (C) Copyright David Vandevoorde and Nicolai M. Josuttis 2002.
 * Permission to copy, use, modify, sell and distribute this software
 * is granted provided this copyright notice appears in all copies.
 * This software is provided "as is" without express or implied
 * warranty, and with no claim as to its suitability for any purpose.
 */
template <typename DstT, typename SrcT>		//!! 把不易推断类型的，如返回值类型，写在前面
inline DstT implicit_cast (SrcT const& x)  // SrcT can be deduced,
{                                          // but DstT cannot
    return x;
}

int main()
{
    double value = implicit_cast<double>(-1);
}
