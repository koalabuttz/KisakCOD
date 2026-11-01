#pragma once
#include "ui_shared.h"
// Exact production parenthesized string literal subset; false means use the general evaluator.
bool UI_TryLiteralExpression(const statement_s *,Operand *);
