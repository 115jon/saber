#ifndef SABER_RESULT_HPP
#define SABER_RESULT_HPP

#include <ekizu/result.hpp>

#define SABER_TRY BOOST_OUTCOME_TRY

namespace saber {
namespace outcome = ekizu::outcome;
template <typename T = boost::blank>
using Result = ekizu::Result<T>;
}  // namespace saber

#endif	// SABER_RESULT_HPP
