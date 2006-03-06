#include "valexpr.h"
#include "walk.h"
#include "error.h"
#include "datetime.h"
#include "debug.h"
#include "util.h"

namespace ledger {

std::auto_ptr<value_calc> amount_expr;
std::auto_ptr<value_calc> total_expr;

std::auto_ptr<scope_t> global_scope;
std::time_t terminus;

details_t::details_t(const transaction_t& _xact)
  : entry(_xact.entry), xact(&_xact), account(xact_account(_xact))
{
  DEBUG_PRINT("ledger.memory.ctors", "ctor details_t");
}

bool compute_amount(value_expr_t * expr, amount_t& amt,
		    const transaction_t * xact, value_expr_t * context)
{
  value_t result;
  expr->compute(result, xact ? details_t(*xact) : details_t(), context);
  switch (result.type) {
  case value_t::BOOLEAN:
    amt = *((bool *) result.data);
    break;
  case value_t::INTEGER:
    amt = *((long *) result.data);
    break;
  case value_t::AMOUNT:
    amt = *((amount_t *) result.data);
    break;

  case value_t::DATETIME:
  case value_t::BALANCE:
  case value_t::BALANCE_PAIR:
    return false;
  }
  return true;
}

value_expr_t::~value_expr_t()
{
  DEBUG_PRINT("ledger.memory.dtors", "dtor value_expr_t " << this);

  DEBUG_PRINT("ledger.valexpr.memory", "Destroying " << this);
  assert(refc == 0);

  if (left)
    left->release();

  switch (kind) {
  case F_CODE_MASK:
  case F_PAYEE_MASK:
  case F_NOTE_MASK:
  case F_ACCOUNT_MASK:
  case F_SHORT_ACCOUNT_MASK:
  case F_COMMODITY_MASK:
    assert(mask);
    delete mask;
    break;

  case CONSTANT_A:
    assert(constant_a);
    delete constant_a;
    break;

  case CONSTANT_T:
    assert(constant_t);
    delete constant_t;
    break;

  case CONSTANT_I:
  case CONSTANT_V:
    break;

  default:
    if (kind > TERMINALS && right)
      right->release();
    break;
  }
}

namespace {
  int count_leaves(value_expr_t * expr)
  {
    int count = 0;
    if (expr->kind != value_expr_t::O_COM) {
      count = 1;
    } else {
      count += count_leaves(expr->left);
      count += count_leaves(expr->right);
    }
    return count;
  }

  value_expr_t * reduce_leaves(value_expr_t * expr, const details_t& details,
			       value_expr_t * context)
  {
    if (! expr)
      return NULL;

    value_auto_ptr temp;

    if (expr->kind != value_expr_t::O_COM) {
      if (expr->kind < value_expr_t::TERMINALS) {
	temp.reset(expr);
      } else {
	temp.reset(new value_expr_t(value_expr_t::CONSTANT_V));
	temp->constant_v = new value_t();
	expr->compute(*(temp->constant_v), details, context);
      }
    } else {
      temp.reset(new value_expr_t(value_expr_t::O_COM));
      temp->set_left(reduce_leaves(expr->left, details, context));
      temp->set_right(reduce_leaves(expr->right, details, context));
    }
    return temp.release();
  }

  value_expr_t * find_leaf(value_expr_t * context, int goal, int& found)
  {
    if (! context)
      return NULL;

    if (context->kind != value_expr_t::O_COM) {
      if (goal == found++)
	return context;
    } else {
      value_expr_t * expr = find_leaf(context->left, goal, found);
      if (expr)
	return expr;
      expr = find_leaf(context->right, goal, found);
      if (expr)
	return expr;
    }
    return NULL;
  }
}

void value_expr_t::compute(value_t& result, const details_t& details,
			   value_expr_t * context) const
{
  switch (kind) {
  case CONSTANT_I:
    result = constant_i;
    break;
  case CONSTANT_T:
    result = *constant_t;
    break;
  case CONSTANT_A:
    result = *constant_a;
    break;
  case CONSTANT_V:
    result = *constant_v;
    break;

  case F_NOW:
    result = datetime_t(terminus);
    break;

  case AMOUNT:
    if (details.xact) {
      if (transaction_has_xdata(*details.xact) &&
	  transaction_xdata_(*details.xact).dflags & TRANSACTION_COMPOSITE)
	result = transaction_xdata_(*details.xact).composite_amount;
      else
	result = details.xact->amount;
    }
    else if (details.account && account_has_xdata(*details.account)) {
      result = account_xdata(*details.account).value;
    }
    else {
      result = 0L;
    }
    break;

  case PRICE:
    if (details.xact) {
      bool set = false;
      if (transaction_has_xdata(*details.xact)) {
	transaction_xdata_t& xdata(transaction_xdata_(*details.xact));
	if (xdata.dflags & TRANSACTION_COMPOSITE) {
	  result = xdata.composite_amount.price();
	  set = true;
	}
      }
      if (! set)
	result = details.xact->amount.price();
    }
    else if (details.account && account_has_xdata(*details.account)) {
      result = account_xdata(*details.account).value.price();
    }
    else {
      result = 0L;
    }
    break;

  case COST:
    if (details.xact) {
      bool set = false;
      if (transaction_has_xdata(*details.xact)) {
	transaction_xdata_t& xdata(transaction_xdata_(*details.xact));
	if (xdata.dflags & TRANSACTION_COMPOSITE) {
	  result = xdata.composite_amount.cost();
	  set = true;
	}
      }

      if (! set) {
	if (details.xact->cost)
	  result = *details.xact->cost;
	else
	  result = details.xact->amount;
      }
    }
    else if (details.account && account_has_xdata(*details.account)) {
      result = account_xdata(*details.account).value.cost();
    }
    else {
      result = 0L;
    }
    break;

  case TOTAL:
    if (details.xact && transaction_has_xdata(*details.xact))
      result = transaction_xdata_(*details.xact).total;
    else if (details.account && account_has_xdata(*details.account))
      result = account_xdata(*details.account).total;
    else
      result = 0L;
    break;
  case PRICE_TOTAL:
    if (details.xact && transaction_has_xdata(*details.xact))
      result = transaction_xdata_(*details.xact).total.price();
    else if (details.account && account_has_xdata(*details.account))
      result = account_xdata(*details.account).total.price();
    else
      result = 0L;
    break;
  case COST_TOTAL:
    if (details.xact && transaction_has_xdata(*details.xact))
      result = transaction_xdata_(*details.xact).total.cost();
    else if (details.account && account_has_xdata(*details.account))
      result = account_xdata(*details.account).total.cost();
    else
      result = 0L;
    break;

  case VALUE_EXPR:
    if (amount_expr.get())
      amount_expr->compute(result, details, context);
    else
      result = 0L;
    break;
  case TOTAL_EXPR:
    if (total_expr.get())
      total_expr->compute(result, details, context);
    else
      result = 0L;
    break;

  case DATE:
    if (details.xact && transaction_has_xdata(*details.xact) &&
	transaction_xdata_(*details.xact).date)
      result = datetime_t(transaction_xdata_(*details.xact).date);
    else if (details.xact)
      result = datetime_t(details.xact->date());
    else if (details.entry)
      result = datetime_t(details.entry->date());
    else
      result = datetime_t(terminus);
    break;

  case ACT_DATE:
    if (details.xact && transaction_has_xdata(*details.xact) &&
	transaction_xdata_(*details.xact).date)
      result = datetime_t(transaction_xdata_(*details.xact).date);
    else if (details.xact)
      result = datetime_t(details.xact->actual_date());
    else if (details.entry)
      result = datetime_t(details.entry->actual_date());
    else
      result = datetime_t(terminus);
    break;

  case EFF_DATE:
    if (details.xact && transaction_has_xdata(*details.xact) &&
	transaction_xdata_(*details.xact).date)
      result = datetime_t(transaction_xdata_(*details.xact).date);
    else if (details.xact)
      result = datetime_t(details.xact->effective_date());
    else if (details.entry)
      result = datetime_t(details.entry->effective_date());
    else
      result = datetime_t(terminus);
    break;

  case CLEARED:
    if (details.xact)
      result = details.xact->state == transaction_t::CLEARED;
    else
      result = false;
    break;
  case PENDING:
    if (details.xact)
      result = details.xact->state == transaction_t::PENDING;
    else
      result = false;
    break;

  case REAL:
    if (details.xact)
      result = ! (details.xact->flags & TRANSACTION_VIRTUAL);
    else
      result = true;
    break;

  case ACTUAL:
    if (details.xact)
      result = ! (details.xact->flags & TRANSACTION_AUTO);
    else
      result = true;
    break;

  case INDEX:
    if (details.xact && transaction_has_xdata(*details.xact))
      result = long(transaction_xdata_(*details.xact).index + 1);
    else if (details.account && account_has_xdata(*details.account))
      result = long(account_xdata(*details.account).count);
    else
      result = 0L;
    break;

  case COUNT:
    if (details.xact && transaction_has_xdata(*details.xact))
      result = long(transaction_xdata_(*details.xact).index + 1);
    else if (details.account && account_has_xdata(*details.account))
      result = long(account_xdata(*details.account).total_count);
    else
      result = 0L;
    break;

  case DEPTH:
    if (details.account)
      result = long(details.account->depth);
    else
      result = 0L;
    break;

  case F_PRICE: {
    int index = 0;
    value_expr_t * expr = find_leaf(context, 0, index);
    expr->compute(result, details, context);
    result = result.price();
    break;
  }

  case F_DATE: {
    int index = 0;
    value_expr_t * expr = find_leaf(context, 0, index);
    expr->compute(result, details, context);
    result = result.date();
    break;
  }

  case F_DATECMP: {
    int index = 0;
    value_expr_t * expr = find_leaf(context, 0, index);
    expr->compute(result, details, context);
    result = result.date();
    if (! result)
      break;

    index = 0;
    expr = find_leaf(context, 1, index);
    value_t moment;
    expr->compute(moment, details, context);
    if (moment.type == value_t::DATETIME) {
      result.cast(value_t::INTEGER);
      moment.cast(value_t::INTEGER);
      result -= moment;
    } else {
      throw compute_error("Invalid date passed to datecmp(value,date)");
    }
    break;
  }

  case F_YEAR:
  case F_MONTH:
  case F_DAY: {
    int index = 0;
    value_expr_t * expr = find_leaf(context, 0, index);
    expr->compute(result, details, context);

    // jww (2006-03-05): Generate an error if result is not a DATETIME
    std::time_t moment = (long)result;
    struct std::tm * desc = std::localtime(&moment);

    switch (kind) {
    case F_YEAR:
      result = (long)desc->tm_year + 1900L;
      break;
    case F_MONTH:
      result = (long)desc->tm_mon + 1L;
      break;
    case F_DAY:
      result = (long)desc->tm_mday;
      break;
    }
    break;
  }

  case F_ARITH_MEAN: {
    int index = 0;
    value_expr_t * expr = find_leaf(context, 0, index);
    if (details.xact && transaction_has_xdata(*details.xact)) {
      expr->compute(result, details, context);
      result /= amount_t(long(transaction_xdata_(*details.xact).index + 1));
    }
    else if (details.account && account_has_xdata(*details.account) &&
	     account_xdata(*details.account).total_count) {
      expr->compute(result, details, context);
      result /= amount_t(long(account_xdata(*details.account).total_count));
    }
    else {
      result = 0L;
    }
    break;
  }

  case F_PARENT:
    if (details.account && details.account->parent)
      left->compute(result, details_t(*details.account->parent), context);
    break;

  case F_ABS: {
    int index = 0;
    value_expr_t * expr = find_leaf(context, 0, index);
    expr->compute(result, details, context);
    result.abs();
    break;
  }

  case F_ROUND: {
    int index = 0;
    value_expr_t * expr = find_leaf(context, 0, index);
    expr->compute(result, details, context);
    result.round();
    break;
  }

  case F_COMMODITY: {
    int index = 0;
    value_expr_t * expr = find_leaf(context, 0, index);
    expr->compute(result, details, context);
    if (result.type != value_t::AMOUNT)
      throw compute_error("Argument to commodity() must be a commoditized amount");
    amount_t temp("1");
    temp.set_commodity(((amount_t *) result.data)->commodity());
    result = temp;
    break;
  }

  case F_SET_COMMODITY: {
    int index = 0;
    value_expr_t * expr = find_leaf(context, 0, index);
    value_t temp;
    expr->compute(temp, details, context);

    index = 0;
    expr = find_leaf(context, 1, index);
    expr->compute(result, details, context);
    if (result.type != value_t::AMOUNT)
      throw compute_error("Second argument to set_commodity() must be a commoditized amount");
    amount_t one("1");
    one.set_commodity(((amount_t *) result.data)->commodity());
    result = one;

    result *= temp;
    break;
  }

  case F_QUANTITY: {
    int index = 0;
    value_expr_t * expr = find_leaf(context, 0, index);
    expr->compute(result, details, context);

    balance_t * bal = NULL;
    switch (result.type) {
    case value_t::BALANCE_PAIR:
      bal = &((balance_pair_t *) result.data)->quantity;
      // fall through...

    case value_t::BALANCE:
      if (! bal)
	bal = (balance_t *) result.data;

      if (bal->amounts.size() < 2) {
	result.cast(value_t::AMOUNT);
      } else {
	value_t temp;
	for (amounts_map::const_iterator i = bal->amounts.begin();
	     i != bal->amounts.end();
	     i++) {
	  amount_t x = (*i).second;
	  x.clear_commodity();
	  temp += x;
	}
	result = temp;
	assert(temp.type == value_t::AMOUNT);
      }
      // fall through...

    case value_t::AMOUNT:
      ((amount_t *) result.data)->clear_commodity();
      break;

    default:
      break;
    }
    break;
  }

  case F_CODE_MASK:
    assert(mask);
    if (details.entry)
      result = mask->match(details.entry->code);
    else
      result = false;
    break;

  case F_PAYEE_MASK:
    assert(mask);
    if (details.entry)
      result = mask->match(details.entry->payee);
    else
      result = false;
    break;

  case F_NOTE_MASK:
    assert(mask);
    if (details.xact)
      result = mask->match(details.xact->note);
    else
      result = false;
    break;

  case F_ACCOUNT_MASK:
    assert(mask);
    if (details.account)
      result = mask->match(details.account->fullname());
    else
      result = false;
    break;

  case F_SHORT_ACCOUNT_MASK:
    assert(mask);
    if (details.account)
      result = mask->match(details.account->name);
    else
      result = false;
    break;

  case F_COMMODITY_MASK:
    assert(mask);
    if (details.xact)
      result = mask->match(details.xact->amount.commodity().symbol());
    else
      result = false;
    break;

  case O_ARG: {
    int index = 0;
    assert(left);
    assert(left->kind == CONSTANT_I);
    value_expr_t * expr = find_leaf(context, left->constant_i, index);
    if (expr)
      expr->compute(result, details, context);
    else
      result = 0L;
    break;
  }

  case O_COM:
    assert(left);
    assert(right);
    left->compute(result, details, context);
    right->compute(result, details, context);
    break;

  case O_DEF:
    throw compute_error("Cannot compute function definition");

  case O_REF: {
    assert(left);
    if (right) {
      value_auto_ptr args(reduce_leaves(right, details, context));
      left->compute(result, details, args.get());
    } else {
      left->compute(result, details, context);
    }
    break;
  }

  case F_VALUE: {
    int index = 0;
    value_expr_t * expr = find_leaf(context, 0, index);
    expr->compute(result, details, context);

    index = 0;
    expr = find_leaf(context, 1, index);
    value_t moment;
    expr->compute(moment, details, context);
    if (moment.type != value_t::DATETIME)
      throw compute_error("Invalid date passed to P(value,date)");

    result = result.value(*((datetime_t *)moment.data));
    break;
  }

  case O_NOT:
    left->compute(result, details, context);
    result.negate();
    break;

  case O_QUES: {
    assert(left);
    assert(right);
    assert(right->kind == O_COL);
    left->compute(result, details, context);
    if (result)
      right->left->compute(result, details, context);
    else
      right->right->compute(result, details, context);
    break;
  }

  case O_AND:
    assert(left);
    assert(right);
    left->compute(result, details, context);
    if (result)
      right->compute(result, details, context);
    break;

  case O_OR:
    assert(left);
    assert(right);
    left->compute(result, details, context);
    if (! result)
      right->compute(result, details, context);
    break;

  case O_NEQ:
  case O_EQ:
  case O_LT:
  case O_LTE:
  case O_GT:
  case O_GTE: {
    assert(left);
    assert(right);
    value_t temp;
    left->compute(temp, details, context);
    right->compute(result, details, context);
    switch (kind) {
    case O_NEQ: result = temp != result; break;
    case O_EQ:  result = temp == result; break;
    case O_LT:  result = temp <  result; break;
    case O_LTE: result = temp <= result; break;
    case O_GT:  result = temp >  result; break;
    case O_GTE: result = temp >= result; break;
    default: assert(0); break;
    }
    break;
  }

  case O_NEG:
    assert(left);
    left->compute(result, details, context);
    result.negate();
    break;

  case O_ADD:
  case O_SUB:
  case O_MUL:
  case O_DIV: {
    assert(left);
    assert(right);
    value_t temp;
    right->compute(temp, details, context);
    left->compute(result, details, context);
    switch (kind) {
    case O_ADD: result += temp; break;
    case O_SUB: result -= temp; break;
    case O_MUL: result *= temp; break;
    case O_DIV: result /= temp; break;
    default: assert(0); break;
    }
    break;
  }

  case O_PERC: {
    assert(left);
    result = "100.0%";
    value_t temp;
    left->compute(temp, details, context);
    result *= temp;
    break;
  }

  case LAST:
  default:
    assert(0);
    break;
  }
}

static inline void unexpected(char c, char wanted = '\0') {
  if ((unsigned char) c == 0xff) {
    if (wanted)
      throw value_expr_error(std::string("Missing '") + wanted + "'");
    else
      throw value_expr_error("Unexpected end");
  } else {
    if (wanted)
      throw value_expr_error(std::string("Invalid char '") + c +
			     "' (wanted '" + wanted + "')");
    else
      throw value_expr_error(std::string("Invalid char '") + c + "'");
  }
}

value_expr_t * parse_value_term(std::istream& in, scope_t * scope);

inline value_expr_t * parse_value_term(const char * p, scope_t * scope) {
  std::istringstream stream(p);
  return parse_value_term(stream, scope);
}

value_expr_t * parse_value_term(std::istream& in, scope_t * scope)
{
  value_auto_ptr node;

  char buf[256];
  char c = peek_next_nonws(in);
  if (std::isdigit(c) || c == '.') {
    READ_INTO(in, buf, 255, c, std::isdigit(c) || c == '.');

    if (std::strchr(buf, '.')) {
      node.reset(new value_expr_t(value_expr_t::CONSTANT_A));
      node->constant_a = new amount_t;
      node->constant_a->parse(buf, AMOUNT_PARSE_NO_MIGRATE);
    } else {
      node.reset(new value_expr_t(value_expr_t::CONSTANT_I));
      node->constant_i = std::atol(buf);
    }
    goto parsed;
  }
  else if (std::isalnum(c) || c == '_') {
    bool have_args = false;
    istream_pos_type beg;

    READ_INTO(in, buf, 255, c, std::isalnum(c) || c == '_');
    c = peek_next_nonws(in);
    if (c == '(') {
      in.get(c);
      beg = in.tellg();

      int paren_depth = 0;
      while (! in.eof()) {
	in.get(c);
	if (c == '(' || c == '{' || c == '[')
	  paren_depth++;
	else if (c == ')' || c == '}' || c == ']') {
	  if (paren_depth == 0)
	    break;
	  paren_depth--;
	}
      }
      if (c != ')')
	unexpected(c, ')');

      have_args = true;
      c = peek_next_nonws(in);
    }

    bool definition = false;
    if (c == '=') {
      in.get(c);
      if (peek_next_nonws(in) == '=') {
	in.unget();
	c = '\0';
      } else {
	definition = true;
      }
    }

    if (definition) {
      std::auto_ptr<scope_t> params(new scope_t(scope));

      int index = 0;
      if (have_args) {
	bool done = false;

	in.clear();
	in.seekg(beg, std::ios::beg);
	while (! done && ! in.eof()) {
	  char ident[32];
	  READ_INTO(in, ident, 31, c, std::isalnum(c) || c == '_');

	  c = peek_next_nonws(in);
	  in.get(c);
	  if (c != ',' && c != ')')
	    unexpected(c, ')');
	  else if (c == ')')
	    done = true;

	  // Define the parameter so that on lookup the parser will find
	  // an O_ARG value.
	  node.reset(new value_expr_t(value_expr_t::O_ARG));
	  node->set_left(new value_expr_t(value_expr_t::CONSTANT_I));
	  node->left->constant_i = index++;
	  params->define(ident, node.release());
	}
	
	if (peek_next_nonws(in) != '=') {
	  in.get(c);
	  unexpected(c, '=');
	}
	in.get(c);
      }

      // Define the value associated with the defined identifier
      value_auto_ptr def(parse_boolean_expr(in, params.get()));
      if (! def.get())
	throw value_expr_error(std::string("Definition failed for '") + buf + "'");

      node.reset(new value_expr_t(value_expr_t::O_DEF));
      node->set_left(new value_expr_t(value_expr_t::CONSTANT_I));
      node->left->constant_i = index;
      node->set_right(def.release());
      
      scope->define(buf, node.release());

      // Returning a dummy value in place of the definition
      node.reset(new value_expr_t(value_expr_t::CONSTANT_I));
      node->constant_i = 0;
    } else {
      assert(scope);
      value_expr_t * def = scope->lookup(buf);
      if (! def) {
	if (buf[1] == '\0' &&
	    (buf[0] == 'c' || buf[0] == 'C' || buf[0] == 'p' ||
	     buf[0] == 'w' || buf[0] == 'W' || buf[0] == 'e'))
	  goto find_term;
	throw value_expr_error(std::string("Unknown identifier '") + buf + "'");
      }
      else if (def->kind == value_expr_t::O_DEF) {
	node.reset(new value_expr_t(value_expr_t::O_REF));
	node->set_left(def->right);

	int count = 0;
	if (have_args) {
	  in.clear();
	  in.seekg(beg, std::ios::beg);
	  value_auto_ptr args(parse_value_expr(in, scope, true));

	  if (peek_next_nonws(in) != ')') {
	    in.get(c);
	    unexpected(c, ')');
	  }
	  in.get(c);

	  if (args.get()) {
	    count = count_leaves(args.get());
	    node->set_right(args.release());
	  }
	}

	if (count != def->left->constant_i) {
	  std::ostringstream errmsg;
	  errmsg << "Wrong number of arguments to '" << buf
		 << "': saw " << count
		 << ", wanted " << def->left->constant_i;
	  throw value_expr_error(errmsg.str());
	}
      }
      else {
	node.reset(def);
      }
    }
    goto parsed;
  }

 find_term:
  in.get(c);
  switch (c) {
  // Functions
  case '^':
    node.reset(new value_expr_t(value_expr_t::F_PARENT));
    node->set_left(parse_value_term(in, scope));
    break;

  // Other
  case 'c':
  case 'C':
  case 'p':
  case 'w':
  case 'W':
  case 'e':
  case '/': {
    bool code_mask	    = c == 'c';
    bool commodity_mask	    = c == 'C';
    bool payee_mask	    = c == 'p';
    bool note_mask	    = c == 'e';
    bool short_account_mask = c == 'w';

    if (c == '/') {
      c = peek_next_nonws(in);
      if (c == '/') {
	in.get(c);
	c = in.peek();
	if (c == '/') {
	  in.get(c);
	  c = in.peek();
	  short_account_mask = true;
	} else {
	  payee_mask = true;
	}
      }
    } else {
      in.get(c);
    }

    // Read in the regexp
    READ_INTO(in, buf, 255, c, c != '/');
    if (c != '/')
      unexpected(c, '/');

    value_expr_t::kind_t kind;

    if (short_account_mask)
      kind = value_expr_t::F_SHORT_ACCOUNT_MASK;
    else if (code_mask)
      kind = value_expr_t::F_CODE_MASK;
    else if (commodity_mask)
      kind = value_expr_t::F_COMMODITY_MASK;
    else if (payee_mask)
      kind = value_expr_t::F_PAYEE_MASK;
    else if (note_mask)
      kind = value_expr_t::F_NOTE_MASK;
    else
      kind = value_expr_t::F_ACCOUNT_MASK;

    in.get(c);
    node.reset(new value_expr_t(kind));
    node->mask = new mask_t(buf);
    break;
  }

  case '{': {
    int paren_depth = 0;
    int i = 0;
    while (i < 255 && ! in.eof()) {
      in.get(c);
      if (c == '{') {
	paren_depth++;
      }
      else if (c == '}') {
	if (paren_depth == 0)
	  break;
	paren_depth--;
      }
      buf[i++] = c;
    }
    buf[i] = '\0';

    if (c != '}')
      unexpected(c, '}');

    node.reset(new value_expr_t(value_expr_t::CONSTANT_A));
    node->constant_a = new amount_t;
    node->constant_a->parse(buf, AMOUNT_PARSE_NO_MIGRATE);
    break;
  }

  case '(': {
    std::auto_ptr<scope_t> locals(new scope_t(scope));
    node.reset(parse_value_expr(in, locals.get(), true));
    in.get(c);
    if (c != ')')
      unexpected(c, ')');
    break;
  }

  case '[': {
    READ_INTO(in, buf, 255, c, c != ']');
    if (c != ']')
      unexpected(c, ']');
    in.get(c);

    node.reset(new value_expr_t(value_expr_t::CONSTANT_T));

    interval_t timespan(buf);
    node->constant_t = new datetime_t(timespan.first());
    break;
  }

  default:
    in.unget();
    break;
  }

 parsed:
  return node.release();
}

value_expr_t * parse_mul_expr(std::istream& in, scope_t * scope)
{
  value_auto_ptr node;

  if (peek_next_nonws(in) == '%') {
    char c;
    in.get(c);
    node.reset(new value_expr_t(value_expr_t::O_PERC));
    node->set_left(parse_value_term(in, scope));
    return node.release();
  }

  node.reset(parse_value_term(in, scope));

  if (node.get() && ! in.eof()) {
    char c = peek_next_nonws(in);
    while (c == '*' || c == '/') {
      in.get(c);
      switch (c) {
      case '*': {
	value_auto_ptr prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_MUL));
	node->set_left(prev.release());
	node->set_right(parse_value_term(in, scope));
	break;
      }

      case '/': {
	value_auto_ptr prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_DIV));
	node->set_left(prev.release());
	node->set_right(parse_value_term(in, scope));
	break;
      }
      }
      c = peek_next_nonws(in);
    }
  }

  return node.release();
}

value_expr_t * parse_add_expr(std::istream& in, scope_t * scope)
{
  value_auto_ptr node;

  if (peek_next_nonws(in) == '-') {
    char c;
    in.get(c);
    value_auto_ptr expr(parse_mul_expr(in, scope));
    if (expr->kind == value_expr_t::CONSTANT_I) {
      expr->constant_i = - expr->constant_i;
      return expr.release();
    }
    else if (expr->kind == value_expr_t::CONSTANT_A) {
      expr->constant_a->negate();
      return expr.release();
    }
    node.reset(new value_expr_t(value_expr_t::O_NEG));
    node->set_left(expr.release());
    return node.release();
  }

  node.reset(parse_mul_expr(in, scope));

  if (node.get() && ! in.eof()) {
    char c = peek_next_nonws(in);
    while (c == '+' || c == '-') {
      in.get(c);
      switch (c) {
      case '+': {
	value_auto_ptr prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_ADD));
	node->set_left(prev.release());
	node->set_right(parse_mul_expr(in, scope));
	break;
      }

      case '-': {
	value_auto_ptr prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_SUB));
	node->set_left(prev.release());
	node->set_right(parse_mul_expr(in, scope));
	break;
      }
      }
      c = peek_next_nonws(in);
    }
  }

  return node.release();
}

value_expr_t * parse_logic_expr(std::istream& in, scope_t * scope)
{
  value_auto_ptr node;

  if (peek_next_nonws(in) == '!') {
    char c;
    in.get(c);
    node.reset(new value_expr_t(value_expr_t::O_NOT));
    node->set_left(parse_add_expr(in, scope));
    return node.release();
  }

  node.reset(parse_add_expr(in, scope));

  if (node.get() && ! in.eof()) {
    char c = peek_next_nonws(in);
    if (c == '!' || c == '=' || c == '<' || c == '>') {
      in.get(c);
      switch (c) {
      case '!':
      case '=': {
	bool negate = c == '!';
	if ((c = peek_next_nonws(in)) == '=')
	  in.get(c);
	else
	  unexpected(c, '=');
	value_auto_ptr prev(node.release());
	node.reset(new value_expr_t(negate ? value_expr_t::O_NEQ :
				    value_expr_t::O_EQ));
	node->set_left(prev.release());
	node->set_right(parse_add_expr(in, scope));
	break;
      }

      case '<': {
	value_auto_ptr prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_LT));
	if (peek_next_nonws(in) == '=') {
	  in.get(c);
	  node->kind = value_expr_t::O_LTE;
	}
	node->set_left(prev.release());
	node->set_right(parse_add_expr(in, scope));
	break;
      }

      case '>': {
	value_auto_ptr prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_GT));
	if (peek_next_nonws(in) == '=') {
	  in.get(c);
	  node->kind = value_expr_t::O_GTE;
	}
	node->set_left(prev.release());
	node->set_right(parse_add_expr(in, scope));
	break;
      }

      default:
	if (! in.eof())
	  unexpected(c);
	break;
      }
    }
  }

  return node.release();
}

value_expr_t * parse_boolean_expr(std::istream& in, scope_t * scope)
{
  value_auto_ptr node(parse_logic_expr(in, scope));

  if (node.get() && ! in.eof()) {
    char c = peek_next_nonws(in);
    while (c == '&' || c == '|' || c == '?') {
      in.get(c);
      switch (c) {
      case '&': {
	value_auto_ptr prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_AND));
	node->set_left(prev.release());
	node->set_right(parse_logic_expr(in, scope));
	break;
      }

      case '|': {
	value_auto_ptr prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_OR));
	node->set_left(prev.release());
	node->set_right(parse_logic_expr(in, scope));
	break;
      }

      case '?': {
	value_auto_ptr prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_QUES));
	node->set_left(prev.release());
	node->set_right(new value_expr_t(value_expr_t::O_COL));
	node->right->set_left(parse_logic_expr(in, scope));
	c = peek_next_nonws(in);
	if (c != ':')
	  unexpected(c, ':');
	in.get(c);
	node->right->set_right(parse_logic_expr(in, scope));
	break;
      }

      default:
	if (! in.eof())
	  unexpected(c);
	break;
      }
      c = peek_next_nonws(in);
    }
  }

  return node.release();
}

void init_value_expr()
{
  global_scope.reset(new scope_t());
  scope_t * globals = global_scope.get();

  value_expr_t * node;

  // Basic terms
  node = new value_expr_t(value_expr_t::F_NOW);
  globals->define("m", node);
  globals->define("now", node);
  globals->define("today", node);

  node = new value_expr_t(value_expr_t::AMOUNT);
  globals->define("a", node);
  globals->define("amount", node);

  node = new value_expr_t(value_expr_t::PRICE);
  globals->define("i", node);
  globals->define("price", node);

  node = new value_expr_t(value_expr_t::COST);
  globals->define("b", node);
  globals->define("cost", node);

  node = new value_expr_t(value_expr_t::DATE);
  globals->define("d", node);
  globals->define("date", node);

  node = new value_expr_t(value_expr_t::ACT_DATE);
  globals->define("act_date", node);
  globals->define("actual_date", node);

  node = new value_expr_t(value_expr_t::EFF_DATE);
  globals->define("eff_date", node);
  globals->define("effective_date", node);

  node = new value_expr_t(value_expr_t::CLEARED);
  globals->define("X", node);
  globals->define("cleared", node);

  node = new value_expr_t(value_expr_t::PENDING);
  globals->define("Y", node);
  globals->define("pending", node);

  node = new value_expr_t(value_expr_t::REAL);
  globals->define("R", node);
  globals->define("real", node);

  node = new value_expr_t(value_expr_t::ACTUAL);
  globals->define("L", node);
  globals->define("actual", node);

  node = new value_expr_t(value_expr_t::INDEX);
  globals->define("n", node);
  globals->define("index", node);

  node = new value_expr_t(value_expr_t::COUNT);
  globals->define("N", node);
  globals->define("count", node);

  node = new value_expr_t(value_expr_t::DEPTH);
  globals->define("l", node);
  globals->define("depth", node);

  node = new value_expr_t(value_expr_t::TOTAL);
  globals->define("O", node);
  globals->define("total", node);

  node = new value_expr_t(value_expr_t::PRICE_TOTAL);
  globals->define("I", node);
  globals->define("total_price", node);

  node = new value_expr_t(value_expr_t::COST_TOTAL);
  globals->define("B", node);
  globals->define("total_cost", node);

  // Relating to format_t
  globals->define("t", new value_expr_t(value_expr_t::VALUE_EXPR));
  globals->define("T", new value_expr_t(value_expr_t::TOTAL_EXPR));

  // Functions
  node = new value_expr_t(value_expr_t::O_DEF);
  node->set_left(new value_expr_t(value_expr_t::CONSTANT_I));
  node->left->constant_i = 1;
  node->set_right(new value_expr_t(value_expr_t::F_ABS));
  globals->define("U", node);
  globals->define("abs", node);

  node = new value_expr_t(value_expr_t::O_DEF);
  node->set_left(new value_expr_t(value_expr_t::CONSTANT_I));
  node->left->constant_i = 1;
  node->set_right(new value_expr_t(value_expr_t::F_ROUND));
  globals->define("round", node);

  node = new value_expr_t(value_expr_t::O_DEF);
  node->set_left(new value_expr_t(value_expr_t::CONSTANT_I));
  node->left->constant_i = 1;
  node->set_right(new value_expr_t(value_expr_t::F_QUANTITY));
  globals->define("S", node);
  globals->define("quant", node);
  globals->define("quantity", node);

  node = new value_expr_t(value_expr_t::O_DEF);
  node->set_left(new value_expr_t(value_expr_t::CONSTANT_I));
  node->left->constant_i = 1;
  node->set_right(new value_expr_t(value_expr_t::F_COMMODITY));
  globals->define("comm", node);
  globals->define("commodity", node);

  node = new value_expr_t(value_expr_t::O_DEF);
  node->set_left(new value_expr_t(value_expr_t::CONSTANT_I));
  node->left->constant_i = 2;
  node->set_right(new value_expr_t(value_expr_t::F_SET_COMMODITY));
  globals->define("setcomm", node);
  globals->define("set_commodity", node);

  node = new value_expr_t(value_expr_t::O_DEF);
  node->set_left(new value_expr_t(value_expr_t::CONSTANT_I));
  node->left->constant_i = 1;
  node->set_right(new value_expr_t(value_expr_t::F_ARITH_MEAN));
  globals->define("A", node);
  globals->define("avg", node);
  globals->define("mean", node);
  globals->define("average", node);

  node = new value_expr_t(value_expr_t::O_DEF);
  node->set_left(new value_expr_t(value_expr_t::CONSTANT_I));
  node->left->constant_i = 2;
  node->set_right(new value_expr_t(value_expr_t::F_VALUE));
  globals->define("P", node);
  value_auto_ptr val(parse_boolean_expr("value=P(t,m)", globals));
  value_auto_ptr tval(parse_boolean_expr("total_value=P(T,m)", globals));
  value_auto_ptr valof(parse_boolean_expr("valueof(x)=P(x,m)", globals));
  value_auto_ptr dvalof(parse_boolean_expr("datedvalueof(x,y)=P(x,y)", globals));

  node = new value_expr_t(value_expr_t::O_DEF);
  node->set_left(new value_expr_t(value_expr_t::CONSTANT_I));
  node->left->constant_i = 1;
  node->set_right(new value_expr_t(value_expr_t::F_PRICE));
  globals->define("priceof", node);

  node = new value_expr_t(value_expr_t::O_DEF);
  node->set_left(new value_expr_t(value_expr_t::CONSTANT_I));
  node->left->constant_i = 1;
  node->set_right(new value_expr_t(value_expr_t::F_DATE));
  globals->define("dateof", node);

  node = new value_expr_t(value_expr_t::O_DEF);
  node->set_left(new value_expr_t(value_expr_t::CONSTANT_I));
  node->left->constant_i = 2;
  node->set_right(new value_expr_t(value_expr_t::F_DATECMP));
  globals->define("datecmp", node);

  node = new value_expr_t(value_expr_t::O_DEF);
  node->set_left(new value_expr_t(value_expr_t::CONSTANT_I));
  node->left->constant_i = 1;
  node->set_right(new value_expr_t(value_expr_t::F_YEAR));
  globals->define("yearof", node);

  node = new value_expr_t(value_expr_t::O_DEF);
  node->set_left(new value_expr_t(value_expr_t::CONSTANT_I));
  node->left->constant_i = 1;
  node->set_right(new value_expr_t(value_expr_t::F_MONTH));
  globals->define("monthof", node);

  node = new value_expr_t(value_expr_t::O_DEF);
  node->set_left(new value_expr_t(value_expr_t::CONSTANT_I));
  node->left->constant_i = 1;
  node->set_right(new value_expr_t(value_expr_t::F_DAY));
  globals->define("dayof", node);

  value_auto_ptr year(parse_boolean_expr("year=yearof(d)", globals));
  value_auto_ptr month(parse_boolean_expr("month=monthof(d)", globals));
  value_auto_ptr day(parse_boolean_expr("day=dayof(d)", globals));

  // Macros
  node = parse_value_expr("P(a,d)");
  globals->define("v", node);
  globals->define("market", node);

  node = parse_value_expr("P(O,d)");
  globals->define("V", node);
  globals->define("total_market", node);

  node = parse_value_expr("v-b");
  globals->define("g", node);
  globals->define("gain", node);

  node = parse_value_expr("V-B");
  globals->define("G", node);
  globals->define("total_gain", node);

  value_auto_ptr minx(parse_boolean_expr("min(x,y)=x<y?x:y", globals));
  value_auto_ptr maxx(parse_boolean_expr("max(x,y)=x>y?x:y", globals));
}

value_expr_t * parse_value_expr(std::istream& in, scope_t * scope,
				const bool partial)
{
  if (! global_scope.get())
    init_value_expr();

  std::auto_ptr<scope_t> this_scope(new scope_t(scope ? scope :
						global_scope.get()));
  value_auto_ptr node;
  node.reset(parse_boolean_expr(in, this_scope.get()));

  if (node.get() && ! in.eof()) {
    char c = peek_next_nonws(in);
    while (c == ',') {
      in.get(c);
      switch (c) {
      case ',': {
	value_auto_ptr prev(node.release());
	node.reset(new value_expr_t(value_expr_t::O_COM));
	node->set_left(prev.release());
	node->set_right(parse_logic_expr(in, this_scope.get()));
	break;
      }

      default:
	if (! in.eof())
	  unexpected(c);
	break;
      }
      c = peek_next_nonws(in);
    }
  }

  char c;
  if (! node.get()) {
    in.get(c);
    if (in.eof())
      throw value_expr_error(std::string("Failed to parse value expression"));
    else
      unexpected(c);
  } else if (! partial) {
    in.get(c);
    if (! in.eof())
      unexpected(c);
    else
      in.unget();
  }

  return node.release();
}

void dump_value_expr(std::ostream& out, const value_expr_t * node,
		     const int depth)
{
  out.setf(std::ios::left);
  out.width(10);
  out << node << " ";

  for (int i = 0; i < depth; i++)
    out << " ";

  switch (node->kind) {
  case value_expr_t::CONSTANT_I:
    out << "CONSTANT_I - " << node->constant_i;
    break;
  case value_expr_t::CONSTANT_T:
    out << "CONSTANT_T - [" << *(node->constant_t) << ']';
    break;
  case value_expr_t::CONSTANT_A:
    out << "CONSTANT_A - {" << *(node->constant_a) << '}';
    break;
  case value_expr_t::CONSTANT_V:
    out << "CONSTANT_V - {" << *(node->constant_v) << '}';
    break;

  case value_expr_t::AMOUNT: out << "AMOUNT"; break;
  case value_expr_t::PRICE: out << "PRICE"; break;
  case value_expr_t::COST: out << "COST"; break;
  case value_expr_t::DATE: out << "DATE"; break;
  case value_expr_t::ACT_DATE: out << "ACT_DATE"; break;
  case value_expr_t::EFF_DATE: out << "EFF_DATE"; break;
  case value_expr_t::CLEARED: out << "CLEARED"; break;
  case value_expr_t::PENDING: out << "PENDING"; break;
  case value_expr_t::REAL: out << "REAL"; break;
  case value_expr_t::ACTUAL: out << "ACTUAL"; break;
  case value_expr_t::INDEX: out << "INDEX"; break;
  case value_expr_t::COUNT: out << "COUNT"; break;
  case value_expr_t::DEPTH: out << "DEPTH"; break;
  case value_expr_t::TOTAL: out << "TOTAL"; break;
  case value_expr_t::PRICE_TOTAL: out << "PRICE_TOTAL"; break;
  case value_expr_t::COST_TOTAL: out << "COST_TOTAL"; break;

  case value_expr_t::F_NOW: out << "F_NOW"; break;
  case value_expr_t::F_ARITH_MEAN: out << "F_ARITH_MEAN"; break;
  case value_expr_t::F_ABS: out << "F_ABS"; break;
  case value_expr_t::F_QUANTITY: out << "F_QUANTITY"; break;
  case value_expr_t::F_COMMODITY: out << "F_COMMODITY"; break;
  case value_expr_t::F_SET_COMMODITY: out << "F_SET_COMMODITY"; break;
  case value_expr_t::F_CODE_MASK: out << "F_CODE_MASK"; break;
  case value_expr_t::F_PAYEE_MASK: out << "F_PAYEE_MASK"; break;
  case value_expr_t::F_NOTE_MASK: out << "F_NOTE_MASK"; break;
  case value_expr_t::F_ACCOUNT_MASK:
    out << "F_ACCOUNT_MASK"; break;
  case value_expr_t::F_SHORT_ACCOUNT_MASK:
    out << "F_SHORT_ACCOUNT_MASK"; break;
  case value_expr_t::F_COMMODITY_MASK:
    out << "F_COMMODITY_MASK"; break;
  case value_expr_t::F_VALUE: out << "F_VALUE"; break;
  case value_expr_t::F_PRICE: out << "F_PRICE"; break;
  case value_expr_t::F_DATE: out << "F_DATE"; break;
  case value_expr_t::F_DATECMP: out << "F_DATECMP"; break;
  case value_expr_t::F_YEAR: out << "F_YEAR"; break;
  case value_expr_t::F_MONTH: out << "F_MONTH"; break;
  case value_expr_t::F_DAY: out << "F_DAY"; break;

  case value_expr_t::O_NOT: out << "O_NOT"; break;
  case value_expr_t::O_ARG: out << "O_ARG"; break;
  case value_expr_t::O_DEF: out << "O_DEF"; break;
  case value_expr_t::O_REF: out << "O_REF"; break;
  case value_expr_t::O_COM: out << "O_COM"; break;
  case value_expr_t::O_QUES: out << "O_QUES"; break;
  case value_expr_t::O_COL: out << "O_COL"; break;
  case value_expr_t::O_AND: out << "O_AND"; break;
  case value_expr_t::O_OR: out << "O_OR"; break;
  case value_expr_t::O_NEQ: out << "O_NEQ"; break;
  case value_expr_t::O_EQ: out << "O_EQ"; break;
  case value_expr_t::O_LT: out << "O_LT"; break;
  case value_expr_t::O_LTE: out << "O_LTE"; break;
  case value_expr_t::O_GT: out << "O_GT"; break;
  case value_expr_t::O_GTE: out << "O_GTE"; break;
  case value_expr_t::O_NEG: out << "O_NEG"; break;
  case value_expr_t::O_ADD: out << "O_ADD"; break;
  case value_expr_t::O_SUB: out << "O_SUB"; break;
  case value_expr_t::O_MUL: out << "O_MUL"; break;
  case value_expr_t::O_DIV: out << "O_DIV"; break;
  case value_expr_t::O_PERC: out << "O_PERC"; break;

  case value_expr_t::LAST:
  default:
    assert(0);
    break;
  }

  out << " (" << node->refc << ')' << std::endl;

  if (node->kind > value_expr_t::TERMINALS) {
    if (node->left) {
      dump_value_expr(out, node->left, depth + 1);
      if (node->right)
	dump_value_expr(out, node->right, depth + 1);
    } else {
      assert(! node->right);
    }
  } else {
    assert(! node->left);
  }
}

} // namespace ledger
