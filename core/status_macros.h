#ifndef SLOP_STATUS_MACROS_H_
#define SLOP_STATUS_MACROS_H_

#define RETURN_IF_ERROR(expr) \
  if (auto _status = (expr); !_status.ok()) return _status

#define ASSIGN_OR_RETURN_IMPL(status_or, lhs, rexpr) \
  auto status_or = (rexpr);                          \
  if (!status_or.ok()) return status_or.status();    \
  lhs = std::move(*status_or)

#define CONCAT_IMPL(x, y) x##y
#define CONCAT(x, y) CONCAT_IMPL(x, y)

#define ASSIGN_OR_RETURN(lhs, rexpr) ASSIGN_OR_RETURN_IMPL(CONCAT(_status_or, __LINE__), lhs, rexpr)

#endif  // SLOP_STATUS_MACROS_H_
