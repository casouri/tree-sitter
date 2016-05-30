#include "runtime/parser.h"
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include "tree_sitter/runtime.h"
#include "tree_sitter/parser.h"
#include "runtime/tree.h"
#include "runtime/lexer.h"
#include "runtime/length.h"
#include "runtime/array.h"
#include "runtime/language.h"
#include "runtime/alloc.h"
#include "runtime/reduce_action.h"

#define LOG(...)                                                               \
  if (self->lexer.debugger.debug_fn) {                                         \
    snprintf(self->lexer.debug_buffer, TS_DEBUG_BUFFER_SIZE, __VA_ARGS__);     \
    self->lexer.debugger.debug_fn(self->lexer.debugger.payload,                \
                                  TSDebugTypeParse, self->lexer.debug_buffer); \
  }

#define LOG_ACTION(...)                   \
  LOG(__VA_ARGS__);                       \
  if (self->print_debugging_graphs) {     \
    fprintf(stderr, "graph {\nlabel=\""); \
    fprintf(stderr, __VA_ARGS__);         \
    fprintf(stderr, "\"\n}\n\n");         \
  }

#define LOG_STACK()                                                     \
  if (self->print_debugging_graphs) {                                   \
    ts_stack_print_dot_graph(self->stack, self->language->symbol_names, \
                             stderr);                                   \
    fputs("\n\n", stderr);                                              \
  }

#define SYM_NAME(symbol) ts_language_symbol_name(self->language, symbol)

#define BOOL_STRING(value) (value ? "true" : "false")

#define CHECK(expr) \
  if (!(expr)) {    \
    goto error;     \
  }

static const TSParseAction ERROR_ACTION = {.type = TSParseActionTypeError };

static const size_t NO_ERROR_DEPTH = (size_t)(-1);

typedef struct {
  TSTree *tree;
  size_t char_index;
} ReusableNode;

typedef struct {
  TSParser *parser;
  TSSymbol lookahead_symbol;
  TreeArray *trees_above_error;
  bool found_repair;
  ReduceAction best_repair;
  TSStateId best_repair_next_state;
  size_t best_repair_skip_count;
} ErrorRepairSession;

typedef enum {
  ParseActionFailed,
  ParseActionUpdated,
  ParseActionRemoved,
} ParseActionResult;

typedef struct {
  enum {
    ReduceFailed,
    ReduceSucceeded,
    ReduceStoppedAtError,
  } status;

  StackSlice slice;
} Reduction;

typedef enum {
  RepairFailed,
  RepairSucceeded,
  RepairNoneFound,
} RepairResult;

typedef enum {
  BreakdownFailed,
  BreakdownPerformed,
  BreakdownAborted,
} BreakdownResult;

static BreakdownResult ts_parser__breakdown_top_of_stack(TSParser *self,
                                                         StackVersion version) {
  TSTree *last_child = NULL;
  bool did_break_down = false;
  bool is_still_pending = false;

  do {
    StackPopResult pop = ts_stack_pop_pending(self->stack, version);
    if (!pop.status)
      goto error;
    if (!pop.slices.size)
      break;

    did_break_down = true;
    is_still_pending = false;
    for (size_t i = 0; i < pop.slices.size; i++) {
      StackSlice slice = pop.slices.contents[i];
      TreeArray removed_trees = slice.trees;
      TSTree *parent = *array_front(&removed_trees);
      LOG("breakdown_pop sym:%s, size:%lu", SYM_NAME(parent->symbol),
          ts_tree_total_size(parent).chars);

      TSStateId state = ts_stack_top_state(self->stack, slice.version);
      for (size_t j = 0; j < parent->child_count; j++) {
        last_child = parent->children[j];
        is_still_pending = last_child->child_count > 0;

        if (last_child->symbol == ts_builtin_sym_error) {
          state = ts_parse_state_error;
        } else if (!last_child->extra) {
          TSParseAction action =
            ts_language_last_action(self->language, state, last_child->symbol);
          assert(action.type == TSParseActionTypeShift);
          state = action.data.to_state;
        }

        LOG("breakdown_push sym:%s, size:%lu", SYM_NAME(last_child->symbol),
            ts_tree_total_size(last_child).chars);
        if (!ts_stack_push(self->stack, slice.version, last_child,
                           is_still_pending, state))
          goto error;
      }

      for (size_t j = 1, count = slice.trees.size; j < count; j++) {
        TSTree *tree = slice.trees.contents[j];
        if (!ts_stack_push(self->stack, slice.version, tree, false, state))
          goto error;
      }

      for (size_t j = 0, count = removed_trees.size; j < count; j++)
        ts_tree_release(removed_trees.contents[j]);
      array_delete(&removed_trees);
    }
  } while (last_child && is_still_pending);

  return did_break_down ? BreakdownPerformed : BreakdownAborted;

error:
  return BreakdownFailed;
}

static void ts_parser__pop_reusable_node(ReusableNode *reusable_node) {
  reusable_node->char_index += ts_tree_total_chars(reusable_node->tree);
  while (reusable_node->tree) {
    TSTree *parent = reusable_node->tree->context.parent;
    size_t next_index = reusable_node->tree->context.index + 1;
    if (parent && parent->child_count > next_index) {
      reusable_node->tree = parent->children[next_index];
      return;
    }
    reusable_node->tree = parent;
  }
}

static void ts_parser__breakdown_reusable_node(ReusableNode *reusable_node) {
  do {
    if (reusable_node->tree->symbol == ts_builtin_sym_error) {
      ts_parser__pop_reusable_node(reusable_node);
      return;
    }

    if (reusable_node->tree->child_count == 0) {
      ts_parser__pop_reusable_node(reusable_node);
      return;
    }

    reusable_node->tree = reusable_node->tree->children[0];
  } while (ts_tree_is_fragile(reusable_node->tree));
}

static bool ts_parser__can_reuse(TSParser *self, StackVersion version,
                                 TSTree *tree) {
  if (tree->symbol == ts_builtin_sym_error)
    return false;

  if (ts_tree_is_fragile(tree) &&
      tree->parse_state != ts_stack_top_state(self->stack, version))
    return false;

  TSStateId top_state = ts_stack_top_state(self->stack, version);

  if (tree->lex_state != TS_TREE_STATE_INDEPENDENT &&
      tree->lex_state != ts_language_lex_state(self->language, top_state))
    return false;

  const TSParseAction action =
    ts_language_last_action(self->language, top_state, tree->symbol);
  if (action.type == TSParseActionTypeError || action.can_hide_split)
    return false;

  if (tree->extra && !action.extra)
    return false;

  return true;
}

static TSTree *ts_parser__lex(TSParser *self, TSStateId state, bool error_mode) {
  TSLexerResult lex_result;
  ts_lexer_start(&self->lexer, state);
  self->language->lex_fn(&self->lexer, state, error_mode);
  ts_lexer_finish(&self->lexer, &lex_result);

  TSTree *result;
  if (lex_result.symbol == ts_builtin_sym_error) {
    result = ts_tree_make_error(lex_result.size, lex_result.padding,
                                lex_result.first_unexpected_character);
  } else {
    LOG("accept_token sym:%s", SYM_NAME(lex_result.symbol));
    result = ts_tree_make_leaf(
      lex_result.symbol, lex_result.padding, lex_result.size,
      ts_language_symbol_metadata(self->language, lex_result.symbol));
  }

  if (!result)
    return NULL;

  if (lex_result.is_fragile)
    result->lex_state = state;

  return result;
}

static TSTree *ts_parser__get_lookahead(TSParser *self, StackVersion version,
                                        ReusableNode *reusable_node) {
  TSLength position = ts_stack_top_position(self->stack, version);

  while (reusable_node->tree) {
    if (reusable_node->char_index > position.chars) {
      break;
    }

    if (reusable_node->char_index < position.chars) {
      LOG("past_reusable sym:%s", SYM_NAME(reusable_node->tree->symbol));
      ts_parser__pop_reusable_node(reusable_node);
      continue;
    }

    if (reusable_node->tree->has_changes) {
      if (reusable_node->tree->child_count == 0)
        ts_parser__breakdown_top_of_stack(self, version);

      LOG("breakdown_changed sym:%s", SYM_NAME(reusable_node->tree->symbol));
      ts_parser__breakdown_reusable_node(reusable_node);
      continue;
    }

    if (!ts_parser__can_reuse(self, version, reusable_node->tree)) {
      LOG("breakdown_unreusable sym:%s", SYM_NAME(reusable_node->tree->symbol));
      ts_parser__breakdown_reusable_node(reusable_node);
      continue;
    }

    TSTree *result = reusable_node->tree;
    TSLength size = ts_tree_total_size(result);
    LOG("reuse sym:%s size:%lu extra:%d", SYM_NAME(result->symbol), size.chars,
        result->extra);
    ts_parser__pop_reusable_node(reusable_node);
    ts_tree_retain(result);
    return result;
  }

  ts_lexer_reset(&self->lexer, position);
  TSStateId parse_state = ts_stack_top_state(self->stack, version);
  bool error_mode = parse_state == ts_parse_state_error;
  TSStateId lex_state = error_mode ? 0 : self->language->lex_states[parse_state];
  LOG("lex state:%d", lex_state);
  return ts_parser__lex(self, lex_state, error_mode);
}

static bool ts_parser__select_tree(TSParser *self, TSTree *left, TSTree *right) {
  if (!left)
    return true;
  if (!right)
    return false;
  if (right->error_size < left->error_size) {
    LOG_ACTION("select_smaller_error symbol:%s, over_symbol:%s",
               SYM_NAME(right->symbol), SYM_NAME(left->symbol));
    return true;
  }
  if (left->error_size < right->error_size) {
    LOG_ACTION("select_smaller_error symbol:%s, over_symbol:%s",
               SYM_NAME(left->symbol), SYM_NAME(right->symbol));
    return false;
  }

  int comparison = ts_tree_compare(left, right);
  switch (comparison) {
    case -1:
      LOG_ACTION("select_earlier symbol:%s, over_symbol:%s",
                 SYM_NAME(left->symbol), SYM_NAME(right->symbol));
      return false;
      break;
    case 1:
      LOG_ACTION("select_earlier symbol:%s, over_symbol:%s",
                 SYM_NAME(right->symbol), SYM_NAME(left->symbol));
      return true;
    default:
      LOG_ACTION("select_existing symbol:%s, over_symbol:%s",
                 SYM_NAME(left->symbol), SYM_NAME(right->symbol));
      return false;
  }
}

static bool ts_parser__push(TSParser *self, StackVersion version, TSTree *tree,
                            TSStateId state) {
  bool result = ts_stack_push(self->stack, version, tree, false, state);
  ts_tree_release(tree);
  return result;
}

static bool ts_parser__shift(TSParser *self, StackVersion version,
                             TSStateId state, TSTree *lookahead, bool extra) {
  if (extra) {
    TSSymbolMetadata metadata =
      ts_language_symbol_metadata(self->language, lookahead->symbol);
    if (metadata.structural && ts_stack_version_count(self->stack) > 1) {
      CHECK(lookahead = ts_tree_make_copy(lookahead));
    } else {
      ts_tree_retain(lookahead);
    }
    lookahead->extra = true;
  } else {
    ts_tree_retain(lookahead);
  }

  bool is_pending = lookahead->child_count > 0;
  CHECK(ts_stack_push(self->stack, version, lookahead, is_pending, state));
  ts_tree_release(lookahead);
  return true;

error:
  ts_tree_release(lookahead);
  return false;
}

static bool ts_parser__switch_children(TSParser *self, TSTree *tree,
                                       TSTree **children, size_t count) {
  self->scratch_tree.symbol = tree->symbol;
  self->scratch_tree.child_count = 0;
  ts_tree_set_children(&self->scratch_tree, count, children);
  if (ts_parser__select_tree(self, tree, &self->scratch_tree)) {
    tree->size = self->scratch_tree.size;
    tree->padding = self->scratch_tree.padding;
    tree->error_size = self->scratch_tree.error_size;
    tree->children = self->scratch_tree.children;
    tree->child_count = self->scratch_tree.child_count;
    tree->named_child_count = self->scratch_tree.named_child_count;
    tree->visible_child_count = self->scratch_tree.visible_child_count;
    return true;
  } else {
    return false;
  }
}

static Reduction ts_parser__reduce(TSParser *self, StackVersion version,
                                   TSSymbol symbol, unsigned count, bool extra,
                                   bool fragile) {
  size_t initial_version_count = ts_stack_version_count(self->stack);
  StackPopResult pop = ts_stack_pop_count(self->stack, version, count);
  switch (pop.status) {
    case StackPopFailed:
      goto error;
    case StackPopStoppedAtError:
      return (Reduction){ ReduceStoppedAtError, pop.slices.contents[0] };
    default:
      break;
  }

  const TSLanguage *language = self->language;
  TSSymbolMetadata metadata = ts_language_symbol_metadata(language, symbol);

  for (size_t i = 0; i < pop.slices.size; i++) {
    StackSlice slice = pop.slices.contents[i];

    size_t child_count = slice.trees.size;
    while (child_count > 0 && slice.trees.contents[child_count - 1]->extra)
      child_count--;

    TSTree *parent =
      ts_tree_make_node(symbol, child_count, slice.trees.contents, metadata);
    if (!parent) {
      ts_tree_array_delete(&slice.trees);
      goto error;
    }

    while (i + 1 < pop.slices.size) {
      StackSlice next_slice = pop.slices.contents[i + 1];
      if (next_slice.version != slice.version)
        break;
      i++;

      size_t child_count = next_slice.trees.size;
      while (child_count > 0 && next_slice.trees.contents[child_count - 1]->extra)
        child_count--;

      if (ts_parser__switch_children(self, parent, next_slice.trees.contents,
                                     child_count)) {
        ts_tree_array_delete(&slice.trees);
        slice = next_slice;
      } else {
        ts_tree_array_delete(&next_slice.trees);
      }
    }

    TSStateId state = ts_stack_top_state(self->stack, slice.version);
    if (fragile || self->is_split || ts_stack_version_count(self->stack) > 1) {
      parent->fragile_left = true;
      parent->fragile_right = true;
      parent->parse_state = TS_TREE_STATE_ERROR;
    } else {
      parent->parse_state = state;
    }

    TSStateId new_state;
    if (extra) {
      parent->extra = true;
      new_state = state;
    } else {
      TSParseAction action = ts_language_last_action(language, state, symbol);
      assert(action.type == TSParseActionTypeShift ||
             action.type == TSParseActionTypeRecover);
      new_state = action.data.to_state;
    }

    CHECK(ts_parser__push(self, slice.version, parent, new_state));
    for (size_t j = parent->child_count; j < slice.trees.size; j++) {
      TSTree *tree = slice.trees.contents[j];
      CHECK(ts_parser__push(self, slice.version, tree, new_state));
    }
  }

  ts_stack_merge_from(self->stack, initial_version_count);

  return (Reduction){ ReduceSucceeded, pop.slices.contents[0] };

error:
  return (Reduction){ ReduceFailed };
}

static bool ts_parser__is_valid_repair(
  const TSParser *self, const TreeArray *trees_below,
  const TreeArray *trees_above, TSStateId start_state, TSSymbol goal_symbol,
  size_t goal_count_below, TSSymbol lookahead_symbol) {
  const TSLanguage *language = self->language;
  TSStateId state = start_state;
  size_t count_below = 0;

  for (size_t i = trees_below->size - 1; i + 1 > 0; i--) {
    TSTree *tree = trees_below->contents[i];
    TSParseAction action =
      ts_language_last_action(language, state, tree->symbol);
    if (action.type != TSParseActionTypeShift)
      return false;
    if (action.extra || tree->extra)
      continue;

    state = action.data.to_state;
    count_below++;

    if (count_below == goal_count_below) {
      for (size_t j = 0; j < trees_above->size; j++) {
        TSTree *tree = trees_above->contents[j];
        TSParseAction action =
          ts_language_last_action(language, state, tree->symbol);
        if (action.type != TSParseActionTypeShift)
          return false;
        if (action.extra || tree->extra)
          continue;

        state = action.data.to_state;
      }

      size_t action_count = 0;
      const TSParseAction *actions =
        ts_language_actions(language, state, lookahead_symbol, &action_count);
      for (size_t k = 0; k < action_count; k++)
        if (actions[k].type == TSParseActionTypeReduce &&
            actions[k].data.symbol == goal_symbol)
          return true;
    }
  }

  return false;
}

static StackIterateAction ts_parser__error_repair_callback(
  void *payload, TSStateId state, TreeArray *trees, size_t tree_count,
  bool is_done, bool is_pending) {
  ErrorRepairSession *session = (ErrorRepairSession *)payload;
  TSParser *self = session->parser;
  const TSLanguage *language = self->language;
  TSSymbol lookahead_symbol = session->lookahead_symbol;
  ReduceActionSet *repairs = &self->reduce_actions;
  TreeArray *trees_above_error = session->trees_above_error;
  StackIterateAction result = StackIterateNone;

  for (size_t i = 0; i < repairs->size; i++) {
    ReduceAction *repair = &repairs->contents[i];
    if (repair->count > tree_count)
      continue;

    size_t skip_count = tree_count - repair->count;
    if (session->found_repair && skip_count >= session->best_repair_skip_count) {
      array_erase(repairs, i--);
      continue;
    }

    TSParseAction repair_symbol_action =
      ts_language_last_action(language, state, repair->symbol);
    if (repair_symbol_action.type != TSParseActionTypeShift)
      continue;

    TSStateId state_after_repair = repair_symbol_action.data.to_state;
    if (!ts_language_has_action(language, state_after_repair, lookahead_symbol))
      continue;

    if (ts_parser__is_valid_repair(self, trees, trees_above_error, state,
                                   repair->symbol, repair->count,
                                   lookahead_symbol)) {
      result |= StackIteratePop;
      session->found_repair = true;
      session->best_repair = *repair;
      session->best_repair_skip_count = skip_count;
      session->best_repair_next_state = state_after_repair;
      array_erase(repairs, i--);
    }
  }

  if (repairs->size == 0)
    result |= StackIterateStop;

  return result;
}

static RepairResult ts_parser__repair_error(TSParser *self, StackSlice slice,
                                            TSTree *lookahead,
                                            const TSParseAction *actions,
                                            size_t action_count) {
  size_t count_above_error = ts_tree_array_essential_count(&slice.trees);
  ErrorRepairSession session = {
    .parser = self,
    .lookahead_symbol = lookahead->symbol,
    .found_repair = false,
    .trees_above_error = &slice.trees,
  };

  array_clear(&self->reduce_actions);
  for (size_t i = 0; i < action_count; i++)
    if (actions[i].type == TSParseActionTypeReduce &&
        actions[i].data.child_count > count_above_error)
      CHECK(array_push(&self->reduce_actions,
                       ((ReduceAction){
                         .symbol = actions[i].data.symbol,
                         .count = actions[i].data.child_count - count_above_error,
                       })));

  StackPopResult pop = ts_stack_iterate(
    self->stack, slice.version, ts_parser__error_repair_callback, &session);
  CHECK(pop.status);

  if (!session.found_repair) {
    LOG_ACTION("no_repair_found");
    ts_stack_remove_version(self->stack, slice.version);
    ts_tree_array_delete(&slice.trees);
    return RepairNoneFound;
  }

  ReduceAction repair = session.best_repair;
  TSStateId next_state = session.best_repair_next_state;
  size_t skip_count = session.best_repair_skip_count;
  TSSymbol symbol = repair.symbol;

  StackSlice new_slice = array_pop(&pop.slices);
  TreeArray children = new_slice.trees;
  ts_stack_renumber_version(self->stack, new_slice.version, slice.version);

  for (size_t i = pop.slices.size - 1; i + 1 > 0; i--) {
    StackSlice other_slice = pop.slices.contents[i];
    ts_tree_array_delete(&other_slice.trees);
    if (other_slice.version != pop.slices.contents[i + 1].version)
      ts_stack_remove_version(self->stack, other_slice.version);
  }

  TreeArray skipped_children = array_new();
  CHECK(array_grow(&skipped_children, skip_count));
  for (size_t i = repair.count; i < children.size; i++)
    array_push(&skipped_children, children.contents[i]);

  TSTree *error = ts_tree_make_error_node(&skipped_children);
  CHECK(error);
  children.size = repair.count;
  array_push(&children, error);

  for (size_t i = 0; i < slice.trees.size; i++)
    array_push(&children, slice.trees.contents[i]);
  array_delete(&slice.trees);

  TSTree *parent =
    ts_tree_make_node(symbol, children.size, children.contents,
                      ts_language_symbol_metadata(self->language, symbol));
  CHECK(parent);
  CHECK(ts_parser__push(self, slice.version, parent, next_state));

  LOG_ACTION("repair_found sym:%s, child_count:%lu, skipped:%lu",
             SYM_NAME(symbol), repair.count + count_above_error,
             parent->error_size);

  return RepairSucceeded;

error:
  ts_tree_array_delete(&slice.trees);
  return RepairFailed;
}

static void ts_parser__start(TSParser *self, TSInput input,
                             TSTree *previous_tree) {
  if (previous_tree) {
    LOG("parse_after_edit");
  } else {
    LOG("new_parse");
  }

  ts_lexer_set_input(&self->lexer, input);
  ts_stack_clear(self->stack);

  self->finished_tree = NULL;
}

static bool ts_parser__accept(TSParser *self, StackVersion version) {
  StackPopResult pop = ts_stack_pop_all(self->stack, version);
  CHECK(pop.status);
  CHECK(pop.slices.size);

  for (size_t i = 0; i < pop.slices.size; i++) {
    StackSlice slice = pop.slices.contents[i];
    TreeArray trees = slice.trees;

    for (size_t j = trees.size - 1; j + 1 > 0; j--) {
      if (!trees.contents[j]->extra) {
        TSTree *root = trees.contents[j];

        CHECK(array_splice(&trees, j, 1, root->child_count, root->children));
        ts_tree_set_children(root, trees.size, trees.contents);
        if (!trees.size)
          array_delete(&trees);

        for (size_t k = j - 1; k + 1 > 0; k--)
          if (!root->children[k]->extra)
            root->error_size += root->children[j]->size.chars;

        if (ts_parser__select_tree(self, self->finished_tree, root)) {
          ts_tree_release(self->finished_tree);
          self->finished_tree = root;
        } else {
          ts_tree_release(root);
        }

        break;
      }
    }
  }

  ts_stack_remove_version(self->stack, pop.slices.contents[0].version);
  ts_stack_remove_version(self->stack, version);

  return true;

error:
  return false;
}

static bool ts_parser__handle_error(TSParser *self, StackVersion version,
                                    TSStateId state, TSTree *lookahead) {
  size_t previous_version_count = ts_stack_version_count(self->stack);

  bool has_shift_action = false;
  array_clear(&self->reduce_actions);
  for (TSSymbol symbol = 0; symbol < self->language->symbol_count; symbol++) {
    size_t action_count;
    const TSParseAction *actions =
      ts_language_actions(self->language, state, symbol, &action_count);

    for (size_t i = 0; i < action_count; i++) {
      TSParseAction action = actions[i];
      if (action.extra)
        continue;
      switch (action.type) {
        case TSParseActionTypeShift:
        case TSParseActionTypeRecover:
          has_shift_action = true;
          break;
        case TSParseActionTypeReduce:
          if (action.data.child_count > 0)
            CHECK(ts_reduce_action_set_add(
              &self->reduce_actions,
              (ReduceAction){
                .symbol = action.data.symbol, .count = action.data.child_count,
              }));
        default:
          break;
      }
    }
  }

  bool did_reduce = false;
  for (size_t i = 0; i < self->reduce_actions.size; i++) {
    ReduceAction action = self->reduce_actions.contents[i];
    Reduction reduction = ts_parser__reduce(self, version, action.symbol,
                                            action.count, false, true);
    switch (reduction.status) {
      case ReduceFailed:
        goto error;
      case ReduceStoppedAtError:
        ts_tree_array_delete(&reduction.slice.trees);
        ts_stack_remove_version(self->stack, reduction.slice.version);
        continue;
      default:
        did_reduce = true;
        break;
    }
  }

  if (did_reduce && !has_shift_action)
    ts_stack_renumber_version(self->stack, previous_version_count, version);

  CHECK(ts_stack_push(self->stack, version, NULL, false, ts_parse_state_error));
  while (ts_stack_version_count(self->stack) > previous_version_count) {
    CHECK(ts_stack_push(self->stack, previous_version_count, NULL, false,
                        ts_parse_state_error));
    assert(ts_stack_merge(self->stack, version, previous_version_count));
  }

  return true;

error:
  return false;
}

static bool ts_parser__recover(TSParser *self, StackVersion version,
                               TSStateId state, TSTree *lookahead) {
  LOG_ACTION("recover state:%u", state);

  StackVersion new_version = ts_stack_duplicate_version(self->stack, version);
  CHECK(new_version != STACK_VERSION_NONE);
  CHECK(ts_parser__shift(
    self, new_version, ts_parse_state_error, lookahead,
    ts_language_symbol_metadata(self->language, lookahead->symbol).extra));

  CHECK(ts_parser__shift(self, version, state, lookahead, false));
  return true;

error:
  return false;
}

static bool ts_parser__recover_eof(TSParser *self, StackVersion version) {
  TreeArray children = array_new();
  TSTree *parent = ts_tree_make_error_node(&children);
  return ts_parser__push(self, version, parent, 1);
}

static ParseActionResult ts_parser__consume_lookahead(TSParser *self,
                                                      StackVersion version,
                                                      TSTree *lookahead) {
  for (;;) {
    TSStateId state = ts_stack_top_state(self->stack, version);

    bool error_repair_failed = false;
    size_t error_repair_depth = NO_ERROR_DEPTH;
    StackVersion last_reduction_version = STACK_VERSION_NONE;

    size_t action_count;
    const TSParseAction *actions = ts_language_actions(
      self->language, state, lookahead->symbol, &action_count);

    for (size_t i = 0;; i++) {
      TSParseAction action;
      if (i < action_count)
        action = actions[i];
      else if (error_repair_failed)
        action = ERROR_ACTION;
      else
        break;

      if (error_repair_depth != NO_ERROR_DEPTH &&
          action.type == TSParseActionTypeReduce &&
          action.data.child_count > error_repair_depth)
        continue;

      LOG_STACK();

      switch (action.type) {
        case TSParseActionTypeError: {
          switch (ts_parser__breakdown_top_of_stack(self, version)) {
            case BreakdownFailed:
              goto error;
            case BreakdownPerformed:
              continue;
            case BreakdownAborted:
              break;
          }

          LOG_ACTION("handle_error");
          CHECK(ts_parser__handle_error(self, version, state, lookahead));
          error_repair_failed = false;
          break;
        }

        case TSParseActionTypeShift: {
          TSStateId next_state;
          if (action.extra) {
            next_state = state;
            LOG_ACTION("shift_extra");
          } else {
            next_state = action.data.to_state;
            LOG_ACTION("shift state:%u", next_state);
          }

          CHECK(ts_parser__shift(self, version, next_state, lookahead,
                                 action.extra));
          return ParseActionUpdated;
        }

        case TSParseActionTypeReduce: {
          if (action.extra) {
            LOG_ACTION("reduce_extra");
          } else {
            LOG_ACTION("reduce sym:%s, child_count:%u, fragile:%s",
                       SYM_NAME(action.data.symbol), action.data.child_count,
                       BOOL_STRING(action.fragile));
          }

          Reduction reduction = ts_parser__reduce(
            self, version, action.data.symbol, action.data.child_count,
            action.extra, action.fragile);

          switch (reduction.status) {
            case ReduceFailed:
              goto error;
            case ReduceSucceeded:
              last_reduction_version = reduction.slice.version;
              break;
            case ReduceStoppedAtError: {
              error_repair_depth =
                ts_tree_array_essential_count(&reduction.slice.trees);

              LOG_ACTION("repair count_above_error:%lu", error_repair_depth);
              switch (ts_parser__repair_error(self, reduction.slice, lookahead,
                                              actions, action_count)) {
                case RepairFailed:
                  goto error;
                case RepairNoneFound:
                  if (last_reduction_version == STACK_VERSION_NONE) {
                    error_repair_failed = true;
                  }
                  break;
                case RepairSucceeded:
                  last_reduction_version = reduction.slice.version;
                  break;
              }
              break;
            }
          }
          break;
        }

        case TSParseActionTypeAccept: {
          LOG_ACTION("accept");
          CHECK(ts_parser__accept(self, version));
          return ParseActionRemoved;
        }

        case TSParseActionTypeRecover: {
          if (lookahead->symbol == ts_builtin_sym_end) {
            LOG_ACTION("recover_eof");
            CHECK(ts_parser__recover_eof(self, version));
          } else {
            CHECK(ts_parser__recover(self, version, action.data.to_state,
                                     lookahead));
          }
          return ParseActionUpdated;
        }
      }
    }

    if (last_reduction_version != STACK_VERSION_NONE)
      ts_stack_renumber_version(self->stack, last_reduction_version, version);
  }

error:
  return ParseActionFailed;
}

bool ts_parser_init(TSParser *self) {
  ts_lexer_init(&self->lexer);
  self->finished_tree = NULL;
  self->stack = NULL;
  array_init(&self->reduce_actions);

  self->stack = ts_stack_new();
  if (!self->stack)
    goto error;

  if (!array_grow(&self->reduce_actions, 4))
    goto error;

  return true;

error:
  if (self->stack) {
    ts_stack_delete(self->stack);
    self->stack = NULL;
  }
  if (self->reduce_actions.contents)
    array_delete(&self->reduce_actions);
  return false;
}

void ts_parser_destroy(TSParser *self) {
  if (self->stack)
    ts_stack_delete(self->stack);
  if (self->reduce_actions.contents)
    array_delete(&self->reduce_actions);
}

TSDebugger ts_parser_debugger(const TSParser *self) {
  return self->lexer.debugger;
}

void ts_parser_set_debugger(TSParser *self, TSDebugger debugger) {
  self->lexer.debugger = debugger;
}

TSTree *ts_parser_parse(TSParser *self, TSInput input, TSTree *previous_tree) {
  ts_parser__start(self, input, previous_tree);
  size_t max_position = 0;
  ReusableNode reusable_node, current_reusable_node = { previous_tree, 0 };

  for (;;) {
    TSTree *lookahead = NULL;
    size_t last_position, position = 0;

    self->is_split = ts_stack_version_count(self->stack) > 1;

    for (StackVersion version = 0;
         version < ts_stack_version_count(self->stack);) {
      reusable_node = current_reusable_node;

      for (bool removed = false; !removed;) {
        last_position = position;
        size_t new_position = ts_stack_top_position(self->stack, version).chars;
        if (new_position > max_position) {
          max_position = new_position;
          version++;
          break;
        } else if (new_position == max_position && version > 0) {
          version++;
          break;
        }

        position = new_position;

        LOG_ACTION("process version:%d, version_count:%lu, state:%d, pos:%lu",
                   version, ts_stack_version_count(self->stack),
                   ts_stack_top_state(self->stack, version), position);

        if (!lookahead || (position != last_position) ||
            !ts_parser__can_reuse(self, version, lookahead)) {
          ts_tree_release(lookahead);
          lookahead = ts_parser__get_lookahead(self, version, &reusable_node);
          if (!lookahead)
            return NULL;
        }

        LOG_ACTION("lookahead sym:%s, size:%lu", SYM_NAME(lookahead->symbol),
                   ts_tree_total_chars(lookahead));

        switch (ts_parser__consume_lookahead(self, version, lookahead)) {
          case ParseActionFailed:
            ts_tree_release(lookahead);
            goto error;
          case ParseActionRemoved:
            removed = true;
            break;
          case ParseActionUpdated:
            break;
        }
      }
    }

    current_reusable_node = reusable_node;

    if (ts_stack_condense(self->stack)) {
      LOG_ACTION("condense");
      LOG_STACK();
    }

    ts_tree_release(lookahead);

    if (ts_stack_version_count(self->stack) == 0) {
      ts_stack_clear(self->stack);
      ts_tree_assign_parents(self->finished_tree);
      return self->finished_tree;
    }
  }

error:
  return NULL;
}
