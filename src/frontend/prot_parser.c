#include "prot_parser.h"

void prot_reinitialize(prot_parser *cp)
{
  if (cp->is_json)
    cp->state.json = initial_json_parser;
  else
    cp->state.sexp = initial_sexp_parser;
}

void prot_initialize(prot_parser *cp, int is_json)
{
  cp->is_json = is_json;
  prot_reinitialize(cp);
}

const char *prot_parse(fz_context *ctx, prot_parser *cp, vstack *stack, const
                       char *input, const char *limit)
{
  if (cp->is_json)
    return json_parse(ctx, &cp->state.json, stack, input, limit);
  else
    return sexp_parse(ctx, &cp->state.sexp, stack, input, limit);
}
