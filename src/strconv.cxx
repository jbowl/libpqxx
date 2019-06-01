/** Implementation of string conversions.
 *
 * Copyright (c) 2000-2019, Jeroen T. Vermeulen.
 *
 * See COPYING for copyright license.  If you did not receive a file called
 * COPYING with this source code, please notify the distributor of this mistake,
 * or contact the author.
 */
#include "pqxx/compiler-internal.hxx"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <locale>
#include <string_view>
#include <system_error>

#if __has_include(<charconv>)
#include <charconv>
#endif

#include "pqxx/except"
#include "pqxx/strconv"


namespace
{
/// String comparison between string_view.
inline bool equal(std::string_view lhs, std::string_view rhs)
{
  return lhs.compare(rhs) == 0;
}


/// Compute numeric value of given textual digit (assuming that it is a digit).
[[maybe_unused]] constexpr int digit_to_number(char c) noexcept
{ return c - '0'; }


/// How big of a buffer do we want for representing a TYPE object as text?
template<typename TYPE> [[maybe_unused]] constexpr int size_buffer()
{
  using lim = std::numeric_limits<TYPE>;
  // Allocate room for how many digits?  There's "max_digits10" for
  // floating-point numbers, but only "digits10" for integer types.
  constexpr auto digits = std::max({lim::digits10, lim::max_digits10});
  // Leave a little bit of extra room for signs, decimal points, and the like.
  return digits + 4;
}
} // namespace


namespace pqxx::internal
{
void throw_null_conversion(const std::string &type)
{
  throw conversion_error{"Attempt to convert null to " + type + "."};
}
} // namespace pqxx::internal


#if defined(PQXX_HAVE_CHARCONV_INT) || defined(PQXX_HAVE_CHARCONV_FLOAT)
template<typename TYPE>
void pqxx::internal::builtin_traits<TYPE>::from_string(
	std::string_view in, TYPE &out)
{
  const char *end = in.data() + in.size();
  const auto res = std::from_chars(in.data(), end, out);
  if (res.ec == std::errc() and res.ptr == end) return;

  std::string msg;
  if (res.ec == std::errc())
  {
    msg = "Could not parse full string.";
  }
  else switch (res.ec)
  {
  case std::errc::result_out_of_range:
    msg = "Value out of range.";
    break;
  case std::errc::invalid_argument:
    msg = "Invalid argument.";
    break;
  default:
    break;
  }

  const std::string base =
	"Could not convert '" + std::string(in) + "' "
	"to " + pqxx::type_name<TYPE>;
  if (msg.empty()) throw pqxx::conversion_error{base + "."};
  else throw pqxx::conversion_error{base + ": " + msg};
}
#endif


#if defined(PQXX_HAVE_CHARCONV_INT) || defined(PQXX_HAVE_CHARCONV_FLOAT)
template<typename T> std::string
pqxx::internal::builtin_traits<T>::to_string(T in)
{
  char buf[size_buffer<T>()];
  const auto res = std::to_chars(buf, buf + sizeof(buf), in);
  if (res.ec == std::errc()) return std::string(buf, res.ptr);

  std::string msg;
  switch (res.ec)
  {
  case std::errc::value_too_large:
    msg = "Value too large.";
    break;
  default:
    break;
  }

  const std::string base =
    std::string{"Could not convert "} + type_name<T> + " to string";
  if (msg.empty()) throw pqxx::conversion_error{base + "."};
  else throw pqxx::conversion_error{base + ": " + msg};
}
#endif // PQXX_HAVE_CHARCONV_INT || PQXX_HAVE_CHARCONV_FLOAT


#if !defined(PQXX_HAVE_CHARCONV_FLOAT)
namespace
{
template<typename T> inline void set_to_Inf(T &t, int sign=1)
{
  T value = std::numeric_limits<T>::infinity();
  if (sign < 0) value = -value;
  t = value;
}
} // namespace
#endif // !PQXX_HAVE_CHARCONV_FLOAT


#if !defined(PQXX_HAVE_CHARCONV_INT)
namespace
{
[[noreturn]] void report_overflow()
{
  throw pqxx::conversion_error{
	"Could not convert string to integer: value out of range."};
}


/// Return 10*n, or throw exception if it overflows.
template<typename T> inline T safe_multiply_by_ten(T n)
{
  using limits = std::numeric_limits<T>;

  constexpr T ten{10};
  constexpr T high_threshold{std::numeric_limits<T>::max() / ten};
  if (n > high_threshold) report_overflow();
  if constexpr (limits::is_signed)
  {
    constexpr T low_threshold{std::numeric_limits<T>::min() / ten};
    if (low_threshold > n) report_overflow();
  }
  return T(n * ten);
}


/// Add digit d to nonnegative n, or throw exception if it overflows.
template<typename T> inline T safe_add_digit(T n, T d)
{
  const T high_threshold{static_cast<T>(std::numeric_limits<T>::max() - d)};
  if (n > high_threshold) report_overflow();
  return static_cast<T>(n + d);
}


/// Subtract digit d to nonpositive n, or throw exception if it overflows.
template<typename T> inline T safe_sub_digit(T n, T d)
{
  const T low_threshold{static_cast<T>(std::numeric_limits<T>::min() + d)};
  if (n < low_threshold) report_overflow();
  return static_cast<T>(n - d);
}


/// For use in string parsing: add new numeric digit to intermediate value.
template<typename L, typename R>
  inline L absorb_digit_positive(L value, R digit)
{
  return safe_add_digit(safe_multiply_by_ten(value), L(digit));
}


/// For use in string parsing: subtract digit from intermediate value.
template<typename L, typename R>
  inline L absorb_digit_negative(L value, R digit)
{
  return safe_sub_digit(safe_multiply_by_ten(value), L(digit));
}


template<typename T> void from_string_signed(std::string_view str, T &obj)
{
  int i = 0;
  T result = 0;

  if (isdigit(str.data()[i]))
  {
    for (; isdigit(str.data()[i]); ++i)
      result = absorb_digit_positive(result, digit_to_number(str.data()[i]));
  }
  else
  {
    if (str.data()[i] != '-')
      throw pqxx::conversion_error{
        "Could not convert string to integer: '" + std::string{str} + "'."};

    for (++i; isdigit(str.data()[i]); ++i)
      result = absorb_digit_negative(result, digit_to_number(str.data()[i]));
  }

  if (str.data()[i])
    throw pqxx::conversion_error{
      "Unexpected text after integer: '" + std::string{str} + "'."};

  obj = result;
}

template<typename T> void from_string_unsigned(std::string_view str, T &obj)
{
  int i = 0;
  T result = 0;

  if (not isdigit(str.data()[i]))
    throw pqxx::conversion_error{
      "Could not convert string to unsigned integer: '" +
      std::string{str} + "'."};

  for (; isdigit(str.data()[i]); ++i)
    result = absorb_digit_positive(result, digit_to_number(str.data()[i]));

  if (str.data()[i])
    throw pqxx::conversion_error{
      "Unexpected text after integer: '" + std::string{str} + "'."};

  obj = result;
}
} // namespace
#endif // !PQXX_HAVE_CHARCONV_INT


#if !defined(PQXX_HAVE_CHARCONV_FLOAT)
namespace
{
bool valid_infinity_string(std::string_view str) noexcept
{
  return
	equal("infinity", str) or
	equal("Infinity", str) or
	equal("INFINITY", str) or
	equal("inf", str);
}
} // namespace
#endif


#if !defined(PQXX_HAVE_CHARCONV_INT) || !defined(PQXX_HAVE_CHARCONV_FLOAT)
namespace
{
/// Wrapper for std::stringstream with C locale.
/** Some of our string conversions use the standard library.  But, they must
 * _not_ obey the system's locale settings, or a value like 1000.0 might end
 * up looking like "1.000,0".
 *
 * Initialising the stream (including locale and tweaked precision) seems to
 * be expensive though.  So, create thread-local instances which we re-use.
 * It's a lockless way of keeping global variables thread-safe, basically.
 *
 * The stream initialisation happens once per thread, in the constructor.
 * And that's why we need to wrap this in a class.  We can't just do it at the
 * call site, or we'd still be doing it for every call.
 */
template<typename T> class dumb_stringstream : public std::stringstream
{
public:
  // Do not initialise the base-class object using "stringstream{}" (with curly
  // braces): that breaks on Visual C++.  The classic "stringstream()" syntax
  // (with parentheses) does work.
  dumb_stringstream()
  {
    this->imbue(std::locale::classic());
    this->precision(std::numeric_limits<T>::max_digits10);
  }
};


template<typename T> [[maybe_unused]] inline
std::string to_string_fallback(T obj)
{
  thread_local dumb_stringstream<T> s;
  s.str("");
  s << obj;
  return s.str();
}
} // namespace
#endif // !PQXX_HAVE_CHARCONV_INT || !PQXX_HAVE_CHARCONV_FLOAT



#if !defined(PQXX_HAVE_CHARCONV_FLOAT)
namespace
{
/* These are hard.  Sacrifice performance of specialized, nonflexible,
 * non-localized code and lean on standard library.  Some special-case code
 * handles NaNs.
 */
template<typename T> inline void from_string_float(
	std::string_view str,
	T &obj)
{
  bool ok = false;
  T result;

  switch (str[0])
  {
  case 'N':
  case 'n':
    // Accept "NaN," "nan," etc.
    ok = (
      (str[1]=='A' or str[1]=='a') and
      (str[2]=='N' or str[2]=='n') and
      (str[3] == '\0'));
    result = std::numeric_limits<T>::quiet_NaN();
    break;

  case 'I':
  case 'i':
    ok = valid_infinity_string(str);
    set_to_Inf(result);
    break;

  default:
    if (str[0] == '-' and valid_infinity_string(&str[1]))
    {
      ok = true;
      set_to_Inf(result, -1);
    }
    else
    {
      thread_local dumb_stringstream<T> S;
      // Visual Studio 2017 seems to fail on repeated conversions if the
      // clear() is done before the seekg().  Still don't know why!  See #124
      // and #125.
      S.seekg(0);
      S.clear();
      // TODO: Inefficient.  Can we get good std::from_chars implementations?
      S.str(std::string{str});
      ok = static_cast<bool>(S >> result);
    }
    break;
  }

  if (not ok)
    throw pqxx::conversion_error{
      "Could not convert string to numeric value: '" +
      std::string{str} + "'."};

  obj = result;
}
} // namespace
#endif // !PQXX_HAVE_CHARCONV_FLOAT


#if !defined(PQXX_HAVE_CHARCONV_INT)
namespace
{
/// Render nonnegative integral value as a string.
/** This is in one way more efficient than what std::to_chars can probably do:
 * since it allocates its own buffer, it can write the digits backwards from
 * the end of the buffer, which is naturally fast in this case.
 *
 * A future string_view-based to_string based on this code could perhaps be
 * more efficient than std::to_chars.
 */
template<typename TYPE> inline std::string to_string_unsigned(
	TYPE obj, bool negative=false)
{
  if (obj == 0) return "0";

  char buf[size_buffer<TYPE>()];
  char *const end = std::end(buf);
  char *begin = end;
  do
  {
    *--begin = pqxx::internal::number_to_digit(int(obj % 10));
    obj = TYPE(obj / 10);
  } while (obj > 0);
  if (negative) *--begin = '-';
  return std::string{begin, end};
}


/// Hard-coded strings for smallest possible integral values.
/** In two's-complement systems (i.e. basically every system these days),
 * any signed integral type has a lowest value which cannot be negated.
 * This complicates rendering those values as strings.  Luckily, there are
 * only a few of these values, so we can just hard-code them at compile time!
 *
 * Another way to do it would be to convert all values to the widest available
 * signed integral type, and only hard-code that type.  But it might widen the
 * division/remainder operations beyond what the processor is comfortable with.
 */
template<long long MIN> constexpr std::string_view minimum{};
#define PQXX_DEFINE_MINIMUM(value) \
	template<> [[maybe_unused]] \
	constexpr std::string_view minimum<value>{#value}
// For signed 8-bit integers:
PQXX_DEFINE_MINIMUM(-128);
// For signed 16-bit integers:
PQXX_DEFINE_MINIMUM(-32768);
// For signed 32-bit integers:
PQXX_DEFINE_MINIMUM(-2147483648);
// For signed 64-bit integers, but somehow too wide for my 64-bit compiler:
// PQXX_DEFINE_MINIMUM(-9223372036854775808);
#undef PQXX_DEFINE_MINIMUM


template<typename T> inline std::string to_string_signed(T obj)
{
  constexpr T bottom{std::numeric_limits<T>::min()};
  if (obj >= 0)
    return to_string_unsigned(obj);
  else if (obj != bottom)
    return to_string_unsigned(-obj, true);
  else if (not minimum<bottom>.empty())
    // The type's minimum value.  Can't negate it without widening.
    return std::string{std::begin(minimum<bottom>), std::end(minimum<bottom>)};
  else
    // The type's minimum value, and we don't have a hard-coded text for it.
    return to_string_fallback(obj);
}
} // namespace
#endif // !PQXX_HAVE_CHARCONV_INT


#if !defined(PQXX_HAVE_CHARCONV_FLOAT)
namespace
{
template<typename T> inline std::string to_string_float(T obj)
{
  if (std::isnan(obj)) return "nan";
  if (std::isinf(obj)) return (obj > 0) ? "infinity" : "-infinity";
  return to_string_fallback(obj);
}
} // namespace
#endif // !PQXX_HAVE_CHARCONV_FLOAT


#if defined(PQXX_HAVE_CHARCONV_INT)
namespace pqxx::internal
{
template void
builtin_traits<short>::from_string(std::string_view, short &);
template void
builtin_traits<unsigned short>::from_string(std::string_view, unsigned short &);
template void
builtin_traits<int>::from_string(std::string_view, int &);
template void
builtin_traits<unsigned int>::from_string(std::string_view, unsigned int &);
template void
builtin_traits<long>::from_string(std::string_view, long &);
template void
builtin_traits<unsigned long>::from_string(std::string_view, unsigned long &);
template void
builtin_traits<long long>::from_string(std::string_view, long long &);
template void
builtin_traits<unsigned long long>::from_string(
	std::string_view, unsigned long long &);
} // namespace pqxx
#endif // PQXX_HAVE_CHARCONV_INT


#if defined(PQXX_HAVE_CHARCONV_FLOAT)
namespace pqxx
{
template
void string_traits<float>::from_string(std::string_view str, float &obj);
template
void string_traits<double>::from_string(std::string_view str, double &obj);
template
void string_traits<long double>::from_string(
	std::string_view str,
	long double &obj);
} // namespace pqxx
#endif // PQXX_HAVE_CHARCONV_FLOAT


#if defined(PQXX_HAVE_CHARCONV_INT)
namespace pqxx::internal
{
template
std::string builtin_traits<short>::to_string(short obj);
template
std::string builtin_traits<unsigned short>::to_string(unsigned short obj);
template
std::string builtin_traits<int>::to_string(int obj);
template
std::string builtin_traits<unsigned int>::to_string(unsigned int obj);
template
std::string builtin_traits<long>::to_string(long obj);
template
std::string builtin_traits<unsigned long>::to_string(unsigned long obj);
template
std::string builtin_traits<long long>::to_string(long long obj);
template
std::string builtin_traits<unsigned long long>::to_string(
	unsigned long long obj);
} // namespace pqxx::internal
#endif // PQXX_HAVE_CHARCONV_INT


#if defined(PQXX_HAVE_CHARCONV_FLOAT)
namespace pqxx::internal
{
template
std::string builtin_traits<float>::to_string(float obj);
template
std::string builtin_traits<double>::to_string(double obj);
template
std::string builtin_traits<long double>::to_string(long double obj);
} // namespace pqxx::internal
#endif // PQXX_HAVE_CHARCONV_FLOAT


#if !defined(PQXX_HAVE_CHARCONV_INT)
namespace pqxx::internal
{
template<>
void builtin_traits<short>::from_string(std::string_view str, short &obj)
	{ from_string_signed(str, obj); }
template<>
std::string builtin_traits<short>::to_string(short obj)
	{ return to_string_signed(obj); }
template<>
void builtin_traits<unsigned short>::from_string(
	std::string_view str,
	unsigned short &obj)
	{ from_string_unsigned(str, obj); }
template<>
std::string builtin_traits<unsigned short>::to_string(unsigned short obj)
	{ return to_string_unsigned(obj); }
template<>
void builtin_traits<int>::from_string(std::string_view str, int &obj)
	{ from_string_signed(str, obj); }
template<>
std::string builtin_traits<int>::to_string(int obj)
	{ return to_string_signed(obj); }
template<>
void builtin_traits<unsigned int>::from_string(
	std::string_view str,
	unsigned int &obj)
	{ from_string_unsigned(str, obj); }
template<>
std::string builtin_traits<unsigned int>::to_string(unsigned int obj)
	{ return to_string_unsigned(obj); }
template<>
void builtin_traits<long>::from_string(std::string_view str, long &obj)
	{ from_string_signed(str, obj); }
template<>
std::string builtin_traits<long>::to_string(long obj)
	{ return to_string_signed(obj); }
template<>
void builtin_traits<unsigned long>::from_string(
	std::string_view str,
	unsigned long &obj)
	{ from_string_unsigned(str, obj); }
template<>
std::string builtin_traits<unsigned long>::to_string(unsigned long obj)
	{ return to_string_unsigned(obj); }
template<>
void builtin_traits<long long>::from_string(
	std::string_view str,
	long long &obj)
	{ from_string_signed(str, obj); }
template<>
std::string builtin_traits<long long>::to_string(long long obj)
	{ return to_string_signed(obj); }
template<>
void builtin_traits<unsigned long long>::from_string(
	std::string_view str,
	unsigned long long &obj)
	{ from_string_unsigned(str, obj); }
template<>
std::string builtin_traits<unsigned long long>::to_string(
        unsigned long long obj)
	{ return to_string_unsigned(obj); }
} // namespace pqxx::internal
#endif // !PQXX_HAVE_CHARCONV_INT


#if !defined(PQXX_HAVE_CHARCONV_FLOAT)
namespace pqxx::internal
{
template<>
void builtin_traits<float>::from_string(std::string_view str, float &obj)
	{ from_string_float(str, obj); }
template<>
std::string builtin_traits<float>::to_string(float obj)
	{ return to_string_float(obj); }
template<>
void builtin_traits<double>::from_string(std::string_view str, double &obj)
	{ from_string_float(str, obj); }
template<>
std::string builtin_traits<double>::to_string(double obj)
	{ return to_string_float(obj); }
template<>
void builtin_traits<long double>::from_string(
	std::string_view str, long double &obj)
	{ from_string_float(str, obj); }
template<>
std::string builtin_traits<long double>::to_string(long double obj)
	{ return to_string_float(obj); }
} // namespace pqxx::internal
#endif // !PQXX_HAVE_CHARCONV_FLOAT


namespace pqxx::internal
{
template<> void builtin_traits<bool>::from_string(
	std::string_view str,
	bool &obj)
{
  bool OK, result;

  switch (str.size())
  {
  case 0:
    result = false;
    OK = true;
    break;

  case 1:
    switch (str[0])
    {
    case 'f':
    case 'F':
    case '0':
      result = false;
      OK = true;
      break;

    case 't':
    case 'T':
    case '1':
      result = true;
      OK = true;
      break;

    default:
      OK = false;
      break;
    }
    break;

  case 4:
    result = true;
    OK = (equal(str, "true") or equal(str, "TRUE"));
    break;

  case 5:
    result = false;
    OK = (equal(str, "false") or equal(str, "FALSE"));
    break;

  default:
    OK = false;
    break;
  }

  if (not OK)
    throw conversion_error{
      "Failed conversion to bool: '" + std::string{str} + "'."};

  obj = result;
}


template<> std::string builtin_traits<bool>::to_string(bool obj)
{
  return obj ? "true" : "false";
}
} // namespace pqxx::internal
