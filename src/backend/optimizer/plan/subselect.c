/*-------------------------------------------------------------------------
 *
 * subselect.c
 *	  Planning routines for subselects and parameters.
 *
 * Portions Copyright (c) 2005-2008, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/optimizer/plan/subselect.c,v 1.141 2008/10/04 21:56:53 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/catalog.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "catalog/gp_policy.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/relation.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/planmain.h"
#include "optimizer/planner.h"
#include "optimizer/prep.h"
#include "optimizer/subselect.h"
#include "optimizer/var.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "parser/parse_oper.h"
#include "rewrite/rewriteManip.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "cdb/cdbmutate.h"
#include "cdb/cdbsubselect.h"
#include "cdb/cdbvars.h"

extern bool is_simple_subquery(PlannerInfo *root, Query *subquery);

typedef struct convert_testexpr_context
{
	PlannerInfo *root;
	List	   *subst_nodes;	/* Nodes to substitute for Params */
} convert_testexpr_context;

typedef struct process_sublinks_context
{
	PlannerInfo *root;
	bool		isTopQual;
} process_sublinks_context;

typedef struct finalize_primnode_context
{
	PlannerInfo *root;
	Bitmapset  *paramids;		/* Non-local PARAM_EXEC paramids found */
} finalize_primnode_context;


static Node *build_subplan(PlannerInfo *root, Plan *plan, List *rtable,
			  SubLinkType subLinkType, Node *testexpr,
			  bool adjust_testexpr, bool unknownEqFalse);

static List *generate_subquery_params(PlannerInfo *root, List *tlist,
						 List **paramIds);
static Node *convert_testexpr_mutator(Node *node,
						 convert_testexpr_context *context);
static bool subplan_is_hashable(PlannerInfo *root, Plan *plan);
static bool testexpr_is_hashable(Node *testexpr);
static bool hash_ok_operator(OpExpr *expr);
static bool simplify_EXISTS_query(PlannerInfo *root, Query *query);
static Query *convert_EXISTS_to_ANY(PlannerInfo *root, Query *subselect,
									Node **testexpr, List **paramIds);
static Node *replace_correlation_vars_mutator(Node *node, PlannerInfo *root);
static Node *process_sublinks_mutator(Node *node,
						 process_sublinks_context *context);
static Bitmapset *finalize_plan(PlannerInfo *root,
			  Plan *plan,
			  Bitmapset *valid_params);
static bool finalize_primnode(Node *node, finalize_primnode_context *context);

extern	double global_work_mem(PlannerInfo *root);

/*
 * Generate a Param node to replace the given Var,
 * which is expected to have varlevelsup > 0 (ie, it is not local).
 */
static Param *
replace_outer_var(PlannerInfo *root, Var *var)
{
	Param	   *retval;
	ListCell   *ppl;
	PlannerParamItem *pitem;
	Index		abslevel;
	int			i;

	Assert(var->varlevelsup > 0 && var->varlevelsup < root->query_level);
	abslevel = root->query_level - var->varlevelsup;

	/*
	 * If there's already a paramlist entry for this same Var, just use it.
	 * NOTE: in sufficiently complex querytrees, it is possible for the same
	 * varno/abslevel to refer to different RTEs in different parts of the
	 * parsetree, so that different fields might end up sharing the same Param
	 * number.	As long as we check the vartype/typmod as well, I believe that
	 * this sort of aliasing will cause no trouble.  The correct field should
	 * get stored into the Param slot at execution in each part of the tree.
	 */
	i = 0;
	foreach(ppl, root->glob->paramlist)
	{
		pitem = (PlannerParamItem *) lfirst(ppl);
		if (pitem->abslevel == abslevel && IsA(pitem->item, Var))
		{
			Var		   *pvar = (Var *) pitem->item;

			if (pvar->varno == var->varno &&
				pvar->varattno == var->varattno &&
				pvar->vartype == var->vartype &&
				pvar->vartypmod == var->vartypmod)
				break;
		}
		i++;
	}

	if (!ppl)
	{
		/* Nope, so make a new one */
		var = (Var *) copyObject(var);
		var->varlevelsup = 0;

		pitem = makeNode(PlannerParamItem);
		pitem->item = (Node *) var;
		pitem->abslevel = abslevel;

		root->glob->paramlist = lappend(root->glob->paramlist, pitem);
		/* i is already the correct index for the new item */
	}

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = i;
	retval->paramtype = var->vartype;
	retval->paramtypmod = var->vartypmod;
	retval->location = -1;

	return retval;
}

/*
 * Generate a Param node to replace the given Aggref
 * which is expected to have agglevelsup > 0 (ie, it is not local).
 */
static Param *
replace_outer_agg(PlannerInfo *root, Aggref *agg)
{
	Param	   *retval;
	PlannerParamItem *pitem;
	Index		abslevel;
	int			i;

	Assert(agg->agglevelsup > 0 && agg->agglevelsup < root->query_level);
	abslevel = root->query_level - agg->agglevelsup;

	/*
	 * It does not seem worthwhile to try to match duplicate outer aggs. Just
	 * make a new slot every time.
	 */
	agg = (Aggref *) copyObject(agg);
	IncrementVarSublevelsUp((Node *) agg, -((int) agg->agglevelsup), 0);
	Assert(agg->agglevelsup == 0);

	pitem = makeNode(PlannerParamItem);
	pitem->item = (Node *) agg;
	pitem->abslevel = abslevel;

	root->glob->paramlist = lappend(root->glob->paramlist, pitem);
	i = list_length(root->glob->paramlist) - 1;

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = i;
	retval->paramtype = agg->aggtype;
	retval->paramtypmod = -1;
	retval->location = -1;

	return retval;
}

/*
 * Generate a new Param node that will not conflict with any other.
 *
 * This is used to allocate PARAM_EXEC slots for subplan outputs.
 */
static Param *
generate_new_param(PlannerInfo *root, Oid paramtype, int32 paramtypmod)
{
	Param	   *retval;
	PlannerParamItem *pitem;

	retval = makeNode(Param);
	retval->paramkind = PARAM_EXEC;
	retval->paramid = list_length(root->glob->paramlist);
	retval->paramtype = paramtype;
	retval->paramtypmod = paramtypmod;
	retval->location = -1;

	pitem = makeNode(PlannerParamItem);
	pitem->item = (Node *) retval;
	pitem->abslevel = root->query_level;

	root->glob->paramlist = lappend(root->glob->paramlist, pitem);

	return retval;
}


/*
 * Assign a (nonnegative) PARAM_EXEC ID for a recursive query's worktable.
 */
int
SS_assign_worktable_param(PlannerInfo *root)
{
	Param	   *param;

	/* We generate a Param of datatype INTERNAL */
	param = generate_new_param(root, INTERNALOID, -1);
	/* ... but the caller only cares about its ID */
	return param->paramid;
}

/*
 * Get the datatype of the first column of the plan's output.
 *
 * This is stored for ARRAY_SUBLINK execution and for exprType()/exprTypmod(),
 * which have no way to get at the plan associated with a SubPlan node.
 * We really only need the info for EXPR_SUBLINK and ARRAY_SUBLINK subplans,
 * but for consistency we save it always.
 */
static void
get_first_col_type(Plan *plan, Oid *coltype, int32 *coltypmod)
{
	/* In cases such as EXISTS, tlist might be empty; arbitrarily use VOID */
	if (plan->targetlist)
	{
		TargetEntry *tent = (TargetEntry *) linitial(plan->targetlist);

		Assert(IsA(tent, TargetEntry));
		if (!tent->resjunk)
		{
			*coltype = exprType((Node *) tent->expr);
			*coltypmod = exprTypmod((Node *) tent->expr);
			return;
		}
	}
	*coltype = VOIDOID;
	*coltypmod = -1;
}

/**
 * Returns true if query refers to a distributed table.
 */
static bool QueryHasDistributedRelation(Query *q)
{
	ListCell   *rt = NULL;

	foreach(rt, q->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(rt);

		if (rte->relid != InvalidOid
				&& rte->rtekind == RTE_RELATION)
		{
			GpPolicy *policy = GpPolicyFetch(CurrentMemoryContext, rte->relid);
			bool result = (policy->ptype == POLICYTYPE_PARTITIONED);
			pfree(policy);
			if (result)
			{
				return true;
			}
		}
	}
	return false;
}

typedef struct CorrelatedVarWalkerContext
{
	int maxLevelsUp;
} CorrelatedVarWalkerContext;

/**
 *  Walker finds the deepest correlation nesting i.e. maximum levelsup among all
 *  vars in subquery.
 */
static bool
CorrelatedVarWalker(Node *node, CorrelatedVarWalkerContext *ctx)
{
	Assert(ctx);

	if (node == NULL)
	{
		return false;
	}
	else if (IsA(node, Var))
	{
		Var * v = (Var *) node;
		if (v->varlevelsup > ctx->maxLevelsUp)
		{
			ctx->maxLevelsUp = v->varlevelsup;
		}
		return false;
	}
	else if (IsA(node, Query))
	{
		return query_tree_walker((Query *) node, CorrelatedVarWalker, ctx, 0 /* flags */);
	}

	return expression_tree_walker(node, CorrelatedVarWalker, ctx);
}

/**
 * Returns true if subquery is correlated
 */
bool
IsSubqueryCorrelated(Query *sq)
{
	Assert(sq);
	CorrelatedVarWalkerContext ctx;
	ctx.maxLevelsUp = 0;
	CorrelatedVarWalker((Node *) sq, &ctx);
	return (ctx.maxLevelsUp > 0);
}

/**
 * Returns true if subquery contains references to more than its immediate outer query.
 */
bool
IsSubqueryMultiLevelCorrelated(Query *sq)
{
	Assert(sq);
	CorrelatedVarWalkerContext ctx;
	ctx.maxLevelsUp = 0;
	CorrelatedVarWalker((Node *) sq, &ctx);
	return (ctx.maxLevelsUp > 1);
}

/*
 * Convert a SubLink (as created by the parser) into a SubPlan.
 *
 * We are given the SubLink's contained query, type, and testexpr.  We are
 * also told if this expression appears at top level of a WHERE/HAVING qual.
 *
 * Note: we assume that the testexpr has been AND/OR flattened (actually,
 * it's been through eval_const_expressions), but not converted to
 * implicit-AND form; and any SubLinks in it should already have been
 * converted to SubPlans.  The subquery is as yet untouched, however.
 *
 * The result is whatever we need to substitute in place of the SubLink
 * node in the executable expression.  This will be either the SubPlan
 * node (if we have to do the subplan as a subplan), or a Param node
 * representing the result of an InitPlan, or a row comparison expression
 * tree containing InitPlan Param nodes.
 */
static Node *
make_subplan(PlannerInfo *root, Query *orig_subquery, SubLinkType subLinkType,
			 Node *testexpr, bool isTopQual)
{
	Query	   *subquery;
	bool		simple_exists = false;
	double		tuple_fraction = 1.0;
	Node		*result;

	/*
	 * Copy the source Query node.	This is a quick and dirty kluge to resolve
	 * the fact that the parser can generate trees with multiple links to the
	 * same sub-Query node, but the planner wants to scribble on the Query.
	 * Try to clean this up when we do querytree redesign...
	 */
	subquery = (Query *) copyObject(orig_subquery);

	/*
 	 * If it's an EXISTS subplan, we might be able to simplify it.
 	 */
	if (subLinkType == EXISTS_SUBLINK)
		simple_exists = simplify_EXISTS_query(root, subquery);
	/*
	 * For an EXISTS subplan, tell lower-level planner to expect that only the
	 * first tuple will be retrieved.  For ALL and ANY subplans, we will be
	 * able to stop evaluating if the test condition fails or matches, so very
	 * often not all the tuples will be retrieved; for lack of a better idea,
	 * specify 50% retrieval.  For EXPR and ROWCOMPARE subplans, use default
	 * behavior (we're only expecting one row out, anyway).
	 *
	 * NOTE: if you change these numbers, also change cost_subplan() in
	 * path/costsize.c.
	 *
	 * XXX If an ANY subplan is uncorrelated, build_subplan may decide to hash
	 * its output.  In that case it would've been better to specify full
	 * retrieval.  At present, however, we can only check hashability after
	 * we've made the subplan :-(.  (Determining whether it'll fit in work_mem
	 * is the really hard part.)  Therefore, we don't want to be too
	 * optimistic about the percentage of tuples retrieved, for fear of
	 * selecting a plan that's bad for the materialization case.
	 */
	if (subLinkType == EXISTS_SUBLINK)
		tuple_fraction = 1.0;	/* just like a LIMIT 1 */
	else if (subLinkType == ALL_SUBLINK ||
			 subLinkType == ANY_SUBLINK)
		tuple_fraction = 0.5;	/* 50% */
	else
		tuple_fraction = 0.0;	/* default behavior */

	PlannerConfig *config = CopyPlannerConfig(root->config);

	if ((Gp_role == GP_ROLE_DISPATCH)
			&& IsSubqueryMultiLevelCorrelated(subquery)
			&& QueryHasDistributedRelation(subquery))
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("correlated subquery with skip-level correlations is not supported")));
	}

	if ((Gp_role == GP_ROLE_DISPATCH)
			&& IsSubqueryCorrelated(subquery)
			&& QueryHasDistributedRelation(subquery))
	{
		/*
		 * Generate the plan for the subquery with certain options disabled.
		 */
		config->gp_enable_direct_dispatch = false;
		config->gp_enable_multiphase_agg = false;

		/*
		 * Only create subplans with sequential scans
		 */
		config->enable_indexscan = false;
		config->enable_bitmapscan = false;
		config->enable_tidscan = false;
		config->enable_seqscan = true;
	}

	if (Gp_role == GP_ROLE_DISPATCH)
	{
		config->gp_cte_sharing = IsSubqueryCorrelated(subquery) ||
				!(subLinkType == ROWCOMPARE_SUBLINK ||
				 subLinkType == ARRAY_SUBLINK ||
				 subLinkType == EXPR_SUBLINK ||
				 subLinkType == EXISTS_SUBLINK);
	}

	/*
	 * Strictly speaking, the order of rows in a subquery doesn't matter.
	 * Consider e.g. "WHERE IN (SELECT ...)". But in case of
	 * "ARRAY(SELECT foo ORDER BY bar)", we'd like to honor the ORDER BY,
	 * and construct the array in that order.
	 */
	if (subLinkType == ARRAY_SUBLINK)
		config->honor_order_by = true;
	else
		config->honor_order_by = false;

	PlannerInfo *subroot = NULL;

	Plan *plan = subquery_planner(root->glob, subquery,
			root,
			false,
			tuple_fraction,
			&subroot,
			config);

	/* And convert to SubPlan or InitPlan format. */
	result = build_subplan(root,
						   plan,
						   subroot->parse->rtable,
						   subLinkType,
						   testexpr,
						   true,
						   isTopQual);

	/*
	 * If it's a correlated EXISTS with an unimportant targetlist, we might be
	 * able to transform it to the equivalent of an IN and then implement it
	 * by hashing.  We don't have enough information yet to tell which way
	 * is likely to be better (it depends on the expected number of executions
	 * of the EXISTS qual, and we are much too early in planning the outer
	 * query to be able to guess that).  So we generate both plans, if
	 * possible, and leave it to the executor to decide which to use.
	 */
	if (simple_exists && IsA(result, SubPlan))
	{
		Node	   *newtestexpr;
		List	   *paramIds;
		
		/* Make a second copy of the original subquery */
		subquery = (Query *) copyObject(orig_subquery);
		/* and re-simplify */
		simple_exists = simplify_EXISTS_query(root, subquery);
		Assert(simple_exists);
		/* See if it can be converted to an ANY query */
		subquery = convert_EXISTS_to_ANY(root, subquery,
										 &newtestexpr, &paramIds);
		if (subquery)
		{
			/* Generate the plan for the ANY subquery; we'll need all rows */
			plan = subquery_planner(root->glob, subquery,
									root,
									false,
									0.0,
									&subroot,
									config);
			
			/* Now we can check if it'll fit in work_mem */
			if (subplan_is_hashable(root, plan))
			{
				SubPlan	   *hashplan;
				AlternativeSubPlan *asplan;
				
				/* OK, convert to SubPlan format. */
				hashplan = (SubPlan *) build_subplan(root, plan,
													 subroot->parse->rtable,
													 ANY_SUBLINK, newtestexpr,
													 false, true);
				/* Check we got what we expected */
				Assert(IsA(hashplan, SubPlan));
				Assert(hashplan->parParam == NIL);
				Assert(hashplan->useHashTable);
				/* build_subplan won't have filled in paramIds */
				hashplan->paramIds = paramIds;
				
				/* Leave it to the executor to decide which plan to use */
				asplan = makeNode(AlternativeSubPlan);
				asplan->subplans = list_make2(result, hashplan);
				result = (Node *) asplan;
			}
		}
	}

	return result;
}

/*
 * Build a SubPlan node given the raw inputs --- subroutine for make_subplan
 *
 * Returns either the SubPlan, or an expression using initplan output Params,
 * as explained in the comments for make_subplan.
 */
static Node *
build_subplan(PlannerInfo *root, Plan *plan, List *rtable,
			  SubLinkType subLinkType, Node *testexpr,
			  bool adjust_testexpr, bool unknownEqFalse)
{
	Node	   *result;
	SubPlan    *splan;
	Bitmapset  *tmpset;
	int			paramid;

	/*
	 * Initialize the SubPlan node.  Note plan_id isn't set till further down,
	 * likewise the cost fields.
	 */
	splan = makeNode(SubPlan);
	splan->subLinkType = subLinkType;
    splan->qDispSliceId = 0;             /*CDB*/
	splan->testexpr = NULL;
	splan->paramIds = NIL;
	get_first_col_type(plan, &splan->firstColType, &splan->firstColTypmod);
	splan->useHashTable = false;
	splan->unknownEqFalse = unknownEqFalse;
	splan->is_initplan = false;
	splan->is_multirow = false;
	splan->is_parallelized = false;
	splan->setParam = NIL;
	splan->parParam = NIL;
	splan->args = NIL;

	/*
	 * Make parParam and args lists of param IDs and expressions that current
	 * query level will pass to this child plan.
	 */
	tmpset = bms_copy(plan->extParam);
	while ((paramid = bms_first_member(tmpset)) >= 0)
	{
		PlannerParamItem *pitem = list_nth(root->glob->paramlist, paramid);
		Node   *arg;

		if (pitem->abslevel == root->query_level)
		{
			splan->parParam = lappend_int(splan->parParam, paramid);
			/*
			 * The Var or Aggref has already been adjusted to have the correct
			 * varlevelsup or agglevelsup.	We probably don't even need to
			 * copy it again, but be safe.
			 */
			arg = copyObject(pitem->item);

			/*
			 * If it's an Aggref, its arguments might contain SubLinks,
			 * which have not yet been processed.  Do that now.
			 */
			if (IsA(arg, Aggref))
				arg = SS_process_sublinks(root, arg, false);

			splan->args = lappend(splan->args, arg);
		}
		else if (pitem->abslevel < root->query_level)
			splan->extParam = lappend_int(splan->extParam, paramid);
	}
	bms_free(tmpset);

	/*
	 * Un-correlated or undirect correlated plans of EXISTS, EXPR, ARRAY, or
	 * ROWCOMPARE types can be used as initPlans.  For EXISTS, EXPR, or ARRAY,
	 * we just produce a Param referring to the result of evaluating the
	 * initPlan.  For ROWCOMPARE, we must modify the testexpr tree to contain
	 * PARAM_EXEC Params instead of the PARAM_SUBLINK Params emitted by the
	 * parser.
	 */
	if (splan->parParam == NIL && subLinkType == EXISTS_SUBLINK && Gp_role == GP_ROLE_DISPATCH)
	{
		Param	   *prm;

		Assert(testexpr == NULL);
		prm = generate_new_param(root, BOOLOID, -1);
		splan->setParam = list_make1_int(prm->paramid);
		splan->is_initplan = true;
		result = (Node *) prm;
	}
	else if (splan->parParam == NIL && subLinkType == EXPR_SUBLINK && Gp_role == GP_ROLE_DISPATCH)
	{
		TargetEntry *te = linitial(plan->targetlist);
		Param	   *prm;

		Assert(!te->resjunk);
		Assert(testexpr == NULL);
		prm = generate_new_param(root,
								 exprType((Node *) te->expr),
								 exprTypmod((Node *) te->expr));
		splan->setParam = list_make1_int(prm->paramid);
		splan->is_initplan = true;
		result = (Node *) prm;
	}
	else if (splan->parParam == NIL && subLinkType == ARRAY_SUBLINK && Gp_role == GP_ROLE_DISPATCH)
	{
		TargetEntry *te = linitial(plan->targetlist);
		Oid			arraytype;
		Param	   *prm;

		Assert(!te->resjunk);
		Assert(testexpr == NULL);
		arraytype = get_array_type(exprType((Node *) te->expr));
		if (!OidIsValid(arraytype))
			elog(ERROR, "could not find array type for datatype %s",
				 format_type_be(exprType((Node *) te->expr)));
		prm = generate_new_param(root,
								 arraytype,
								 exprTypmod((Node *) te->expr));
		splan->setParam = list_make1_int(prm->paramid);
		splan->is_initplan = true;
		result = (Node *) prm;
	}
	else if (splan->parParam == NIL && subLinkType == ROWCOMPARE_SUBLINK && Gp_role == GP_ROLE_DISPATCH)
	{
		/* Adjust the Params */
		List	   *params;

		params = generate_subquery_params(root,
										  plan->targetlist,
										  &splan->paramIds);
		result = convert_testexpr(root,
								  testexpr,
								  params);
		splan->setParam = list_copy(splan->paramIds);
		splan->is_initplan = true;

		/*
		 * The executable expression is returned to become part of the outer
		 * plan's expression tree; it is not kept in the initplan node.
		 */
	}
	else
	{
		/*
		 * Adjust the Params in the testexpr, unless caller said it's not
		 * needed.
		 */
		if (testexpr && adjust_testexpr)
		{
			List	   *params;

			params = generate_subquery_params(root,
											  plan->targetlist,
											  &splan->paramIds);
			splan->testexpr = convert_testexpr(root,
											   testexpr,
											   params);
		}
		else
			splan->testexpr = testexpr;

		splan->is_multirow = true; /* CDB: take note. */

		/*
		 * We can't convert subplans of ALL_SUBLINK or ANY_SUBLINK types to
		 * initPlans, even when they are uncorrelated or undirect correlated,
		 * because we need to scan the output of the subplan for each outer
		 * tuple.  But if it's a not-direct-correlated IN (= ANY) test, we
		 * might be able to use a hashtable to avoid comparing all the tuples.
		 * TODO siva - I believe we should've pulled these up to be NL joins.
		 * We may want to assert that this is never exercised.
		 */
		if (subLinkType == ANY_SUBLINK &&
			splan->parParam == NIL &&
			subplan_is_hashable(root, plan) &&
			testexpr_is_hashable(splan->testexpr))
			splan->useHashTable = true;

		result = (Node *) splan;
	}

	AssertEquivalent(splan->is_initplan, !splan->is_multirow && splan->parParam == NIL);

	/*
	 * Add the subplan and its rtable to the global lists.
	 */
	root->glob->subplans = lappend(root->glob->subplans,
								   plan);
	root->glob->subrtables = lappend(root->glob->subrtables,
									 rtable);
	splan->plan_id = list_length(root->glob->subplans);

	if (splan->is_initplan)
		root->init_plans = lappend(root->init_plans, splan);

	/*
	 * A parameterless subplan (not initplan) should be prepared to handle
	 * REWIND efficiently.	If it has direct parameters then there's no point
	 * since it'll be reset on each scan anyway; and if it's an initplan then
	 * there's no point since it won't get re-run without parameter changes
	 * anyway.	The input of a hashed subplan doesn't need REWIND either.
	 */
	if (splan->parParam == NIL && !splan->is_initplan && !splan->useHashTable)
		root->glob->rewindPlanIDs = bms_add_member(root->glob->rewindPlanIDs,
												   splan->plan_id);

	/* Label the subplan for EXPLAIN purposes */
	if (splan->is_initplan)
	{
		ListCell   *lc;
		
		StringInfo buf = makeStringInfo();
		
		appendStringInfo(buf, "InitPlan %d (returns ", splan->plan_id);
		
		foreach(lc, splan->setParam)
		{
			appendStringInfo(buf, "$%d%s",
							lfirst_int(lc),
							lnext(lc) ? "," : "");
		}
		appendStringInfoString(buf, ")");
		splan->plan_name = pstrdup(buf->data);
		pfree(buf->data);
		pfree(buf);
		buf = NULL;
	}
	else
	{
		StringInfo buf = makeStringInfo();
		appendStringInfo(buf, "SubPlan %d", splan->plan_id);
		splan->plan_name = pstrdup(buf->data);
		pfree(buf->data);
		pfree(buf);
		buf = NULL;
	}

	cost_subplan(root, splan, plan);

	return result;
}

/*
 * generate_subquery_params: build a list of Params representing the output
 * columns of a sublink's sub-select, given the sub-select's targetlist.
 *
 * We also return an integer list of the paramids of the Params.
 */
static List *
generate_subquery_params(PlannerInfo *root, List *tlist, List **paramIds)
{
	List	   *result;
	List	   *ids;
	ListCell   *lc;

	result = ids = NIL;
	foreach(lc, tlist)
	{
		TargetEntry *tent = (TargetEntry *) lfirst(lc);
		Param	   *param;

		if (tent->resjunk)
			continue;

		param = generate_new_param(root,
								   exprType((Node *) tent->expr),
								   exprTypmod((Node *) tent->expr));
		result = lappend(result, param);
		ids = lappend_int(ids, param->paramid);
	}

	*paramIds = ids;
	return result;
}

/*
 * generate_subquery_vars: build a list of Vars representing the output
 * columns of a sublink's sub-select, given the sub-select's targetlist.
 * The Vars have the specified varno (RTE index).
 */
List *
generate_subquery_vars(PlannerInfo *root, List *tlist, Index varno)
{
	List	   *result;
	ListCell   *lc;

	result = NIL;
	foreach(lc, tlist)
	{
		TargetEntry *tent = (TargetEntry *) lfirst(lc);
		Var		   *var;

		if (tent->resjunk)
			continue;

		var = makeVar(varno,
					  tent->resno,
					  exprType((Node *) tent->expr),
					  exprTypmod((Node *) tent->expr),
					  0);
		result = lappend(result, var);
	}

	return result;
}

/*
 * convert_testexpr: convert the testexpr given by the parser into
 * actually executable form.  This entails replacing PARAM_SUBLINK Params
 * with Params or Vars representing the results of the sub-select.  The
 * nodes to be substituted are passed in as the List result from
 * generate_subquery_params or generate_subquery_vars.
 *
 * The given testexpr has already been recursively processed by
 * process_sublinks_mutator.  Hence it can no longer contain any
 * PARAM_SUBLINK Params for lower SubLink nodes; we can safely assume that
 * any we find are for our own level of SubLink.
 */
Node *
convert_testexpr(PlannerInfo *root,
				 Node *testexpr,
				 List *subst_nodes)
{
	convert_testexpr_context context;

	context.root = root;
	context.subst_nodes = subst_nodes;
	return convert_testexpr_mutator(testexpr, &context);
}

static Node *
convert_testexpr_mutator(Node *node,
						 convert_testexpr_context *context)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Param))
	{
		Param	   *param = (Param *) node;

		if (param->paramkind == PARAM_SUBLINK)
		{
			if (param->paramid <= 0 ||
				param->paramid > list_length(context->subst_nodes))
				elog(ERROR, "unexpected PARAM_SUBLINK ID: %d", param->paramid);

			/*
			 * We copy the list item to avoid having doubly-linked
			 * substructure in the modified parse tree.  This is probably
			 * unnecessary when it's a Param, but be safe.
			 */
			return (Node *) copyObject(list_nth(context->subst_nodes,
												param->paramid - 1));
		}
	}
	return expression_tree_mutator(node,
								   convert_testexpr_mutator,
								   (void *) context);
}

/*
 * subplan_is_hashable: can we implement an ANY subplan by hashing?
 */
static bool
subplan_is_hashable(PlannerInfo *root, Plan *plan)
{
	double		subquery_size;

	/*
	 * The estimated size of the subquery result must fit in work_mem. (Note:
	 * we use sizeof(HeapTupleHeaderData) here even though the tuples will
	 * actually be stored as MinimalTuples; this provides some fudge factor
	 * for hashtable overhead.)
	 */
	subquery_size = plan->plan_rows *
		(MAXALIGN(plan->plan_width) + MAXALIGN(sizeof(HeapTupleHeaderData)));
	if (subquery_size > global_work_mem(root))
		return false;

	return true;
}

/*
 * testexpr_is_hashable: is an ANY SubLink's test expression hashable?
 */
static bool
testexpr_is_hashable(Node *testexpr)
{
	/*
	 * The testexpr must be a single OpExpr, or an AND-clause containing
	 * only OpExprs.
	 *
	 * The combining operators must be hashable and strict. The need for
	 * hashability is obvious, since we want to use hashing. Without
	 * strictness, behavior in the presence of nulls is too unpredictable.	We
	 * actually must assume even more than plain strictness: they can't yield
	 * NULL for non-null inputs, either (see nodeSubplan.c).  However, hash
	 * indexes and hash joins assume that too.
	 */
	if (testexpr && IsA(testexpr, OpExpr))
	{
		if (hash_ok_operator((OpExpr *) testexpr))
			return true;
	}
	else if (and_clause(testexpr))
	{
		ListCell   *l;

		foreach(l, ((BoolExpr *) testexpr)->args)
		{
			Node	   *andarg = (Node *) lfirst(l);

			if (!IsA(andarg, OpExpr))
				return false;
			if (!hash_ok_operator((OpExpr *) andarg))
				return false;
		}
		return true;
	}

	return false;
}

static bool
hash_ok_operator(OpExpr *expr)
{
	Oid			opid = expr->opno;
	HeapTuple	tup;
	Form_pg_operator optup;

	/* quick out if not a binary operator */
	if (list_length(expr->args) != 2)
		return false;
	/* else must look up the operator properties */
	tup = SearchSysCache(OPEROID,
						 ObjectIdGetDatum(opid),
						 0, 0, 0);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for operator %u", opid);
	optup = (Form_pg_operator) GETSTRUCT(tup);
	if (!optup->oprcanhash || !func_strict(optup->oprcode))
	{
		ReleaseSysCache(tup);
		return false;
	}
	ReleaseSysCache(tup);
	return true;
}


/*
 * SS_process_ctes: process a query's WITH list
 *
 * We plan each interesting WITH item and convert it to an initplan.
 * A side effect is to fill in root->cte_plan_ids with a list that
 * parallels root->parse->cteList and provides the subplan ID for
 * each CTE's initplan.
 */
void
SS_process_ctes(PlannerInfo *root)
{
	ListCell   *lc;

	Assert(root->cte_plan_ids == NIL);

	foreach(lc, root->parse->cteList)
	{
		CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);
		Query	   *subquery;
		Plan	   *plan;
		PlannerInfo *subroot;
		SubPlan    *splan;
		Bitmapset  *tmpset;
		int			paramid;
		Param	   *prm;

		/*
		 * Ignore CTEs that are not actually referenced anywhere.
		 */
		if (cte->cterefcount == 0)
		{
			/* Make a dummy entry in cte_plan_ids */
			root->cte_plan_ids = lappend_int(root->cte_plan_ids, -1);
			continue;
		}

		/*
		 * Copy the source Query node.  Probably not necessary, but let's
		 * keep this similar to make_subplan.
		 */
		subquery = (Query *) copyObject(cte->ctequery);

		/*
		 * Generate the plan for the CTE query.  Always plan for full
		 * retrieval --- we don't have enough info to predict otherwise.
		 */
		plan = subquery_planner(root->glob, subquery,
								root,
								cte->cterecursive, 0.0,
								&subroot,
								root->config);

		/*
		 * Make a SubPlan node for it.  This is just enough unlike
		 * build_subplan that we can't share code.
		 *
		 * Note plan_id isn't set till further down, likewise the cost fields.
		 */
		splan = makeNode(SubPlan);
		splan->subLinkType = CTE_SUBLINK;
		splan->testexpr = NULL;
		splan->paramIds = NIL;
		get_first_col_type(plan, &splan->firstColType, &splan->firstColTypmod);
		splan->useHashTable = false;
		splan->unknownEqFalse = false;
		splan->setParam = NIL;
		splan->parParam = NIL;
		splan->args = NIL;

		/*
		 * Make parParam and args lists of param IDs and expressions that
		 * current query level will pass to this child plan.  Even though
		 * this is an initplan, there could be side-references to earlier
		 * initplan's outputs, specifically their CTE output parameters.
		 */
		tmpset = bms_copy(plan->extParam);
		while ((paramid = bms_first_member(tmpset)) >= 0)
		{
			PlannerParamItem *pitem = list_nth(root->glob->paramlist, paramid);

			if (pitem->abslevel == root->query_level)
			{
				prm = (Param *) pitem->item;
				if (!IsA(prm, Param) ||
					prm->paramtype != INTERNALOID)
					elog(ERROR, "bogus local parameter passed to WITH query");

				splan->parParam = lappend_int(splan->parParam, paramid);
				splan->args = lappend(splan->args, copyObject(prm));
			}
		}
		bms_free(tmpset);

		/*
		 * Assign a param to represent the query output.  We only really
		 * care about reserving a parameter ID number.
		 */
		prm = generate_new_param(root, INTERNALOID, -1);
		splan->setParam = list_make1_int(prm->paramid);

		/*
		 * Add the subplan and its rtable to the global lists.
		 */
		root->glob->subplans = lappend(root->glob->subplans, plan);
		root->glob->subrtables = lappend(root->glob->subrtables,
										 subroot->parse->rtable);
		splan->plan_id = list_length(root->glob->subplans);

		root->init_plans = lappend(root->init_plans, splan);

		root->cte_plan_ids = lappend_int(root->cte_plan_ids, splan->plan_id);

		/* Lastly, fill in the cost estimates for use later */
		// FIXME : CTE_MERGE: cost_subplan is part of the commit bd3daddaf232d95b0c9ba6f99b0170a0147dd8af
		//cost_subplan(root, splan, plan);
	}
}

/*
 * convert_ANY_sublink_to_join: try to convert an ANY SubLink to a join
 *
 * The caller has found an ANY SubLink at the top level of one of the query's
 * qual clauses, but has not checked the properties of the SubLink further.
 * Decide whether it is appropriate to process this SubLink in join style.
 * If so, form a JoinExpr and return it.  Return NULL if the SubLink cannot
 * be converted to a join.
 *
 * The only non-obvious input parameter is available_rels: this is the set
 * of query rels that can safely be referenced in the sublink expression.
 * (We must restrict this to avoid changing the semantics when a sublink
 * is present in an outer join's ON qual.)  The conversion must fail if
 * the converted qual would reference any but these parent-query relids.
 *
 * On success, the returned JoinExpr has larg = NULL and rarg = the jointree
 * item representing the pulled-up subquery.  The caller must set larg to
 * represent the relation(s) on the lefthand side of the new join, and insert
 * the JoinExpr into the upper query's jointree at an appropriate place
 * (typically, where the lefthand relation(s) had been).  Note that the
 * passed-in SubLink must also be removed from its original position in the
 * query quals, since the quals of the returned JoinExpr replace it.
 * (Notionally, we replace the SubLink with a constant TRUE, then elide the
 * redundant constant from the qual.)
 *
 * Side effects of a successful conversion include adding the SubLink's
 * subselect to the query's rangetable, so that it can be referenced in
 * the JoinExpr's rarg.
 */
JoinExpr *
convert_ANY_sublink_to_join(PlannerInfo *root, SubLink *sublink,
							Relids available_rels)
{
	JoinExpr   *result;
	Query		*parse = root->parse;
	Query		*subselect = (Query *) sublink->subselect;
	Relids		upper_varnos;
	int			rtindex;
	RangeTblEntry *rte;
	RangeTblRef *rtr;
	List		*subquery_vars;
	Node		*quals;
	bool		correlated;

	Assert(sublink->subLinkType == ANY_SUBLINK);
	Assert(IsA(subselect, Query));

	cdbsubselect_drop_orderby(subselect);
	cdbsubselect_drop_distinct(subselect);

	/*
	 * If subquery returns a set-returning function (SRF) in the targetlist, we
	 * do not attempt to convert the IN to a join.
	 */
	if (expression_returns_set((Node *) subselect->targetList))
		return NULL;

	/*
	 * If deeply correlated, then don't pull it up
	 */
	if (IsSubqueryMultiLevelCorrelated(subselect))
		return NULL;

	/*
	 * If there are CTEs, then the transformation does not work. Don't attempt
	 * to pullup.
	 */
	if (parse->cteList)
		return NULL;

	/*
	 * If uncorrelated, and no Var nodes on lhs, the subquery will be executed
	 * only once.  It should become an InitPlan, but make_subplan() doesn't
	 * handle that case, so just flatten it for now.
	 * CDB TODO: Let it become an InitPlan, so its QEs can be recycled.
	 */
	correlated = contain_vars_of_level_or_above(sublink->subselect, 1);

	if (correlated)
	{
		/*
		 * Under certain conditions, we cannot pull up the subquery as a join.
		 */
		if (!is_simple_subquery(root, subselect))
			return NULL;

		/*
		 * Do not pull subqueries with correlation in a func expr in the from
		 * clause of the subselect
		 */
		if (has_correlation_in_funcexpr_rte(subselect->rtable))
			return NULL;

		if (contain_subplans(subselect->jointree->quals))
			return NULL;
	}

	/*
	 * The test expression must contain some Vars of the parent query,
	 * else it's not gonna be a join.  (Note that it won't have Vars
	 * referring to the subquery, rather Params.)
	 */
	upper_varnos = pull_varnos(sublink->testexpr);
	if (bms_is_empty(upper_varnos))
		return NULL;

	/*
	 * However, it can't refer to anything outside available_rels.
	 */
	if (!bms_is_subset(upper_varnos, available_rels))
		return NULL;

	/*
	 * The combining operators and left-hand expressions mustn't be volatile.
	 */
	if (contain_volatile_functions(sublink->testexpr))
		return NULL;

	/*
	 * Okay, pull up the sub-select into upper range table.
	 *
	 * We rely here on the assumption that the outer query has no references
	 * to the inner (necessarily true, other than the Vars that we build
	 * below). Therefore this is a lot easier than what pull_up_subqueries has
	 * to go through.
	 */
	rte = addRangeTableEntryForSubquery(NULL,
										subselect,
										makeAlias("ANY_subquery", NIL),
										false);
	parse->rtable = lappend(parse->rtable, rte);
	rtindex = list_length(parse->rtable);

	/*
	 * Form a RangeTblRef for the pulled-up sub-select.
	 */
	rtr = makeNode(RangeTblRef);
	rtr->rtindex = rtindex;

	/*
	 * Build a list of Vars representing the subselect outputs.
	 */
	subquery_vars = generate_subquery_vars(root,
										   subselect->targetList,
										   rtindex);

	/*
	 * Build the new join's qual expression, replacing Params with these Vars.
	 */
	quals = convert_testexpr(root, sublink->testexpr, subquery_vars);

	result = makeNode(JoinExpr);
	result->jointype = JOIN_SEMI;
	result->isNatural = false;
	result->larg = NULL;		/* caller must fill this in */
	result->rarg = (Node *) rtr;
	result->quals = quals;
	result->alias = NULL;
	result->rtindex = 0;

	return result;
}

/*
 * simplify_EXISTS_query: remove any useless stuff in an EXISTS's subquery
 *
 * The only thing that matters about an EXISTS query is whether it returns
 * zero or more than zero rows.  Therefore, we can remove certain SQL features
 * that won't affect that.  The only part that is really likely to matter in
 * typical usage is simplifying the targetlist: it's a common habit to write
 * "SELECT * FROM" even though there is no need to evaluate any columns.
 *
 * Note: by suppressing the targetlist we could cause an observable behavioral
 * change, namely that any errors that might occur in evaluating the tlist
 * won't occur, nor will other side-effects of volatile functions.  This seems
 * unlikely to bother anyone in practice.
 *
 * Returns TRUE if was able to discard the targetlist, else FALSE.
 */
static bool
simplify_EXISTS_query(PlannerInfo *root, Query *query)
{
	if (!is_simple_subquery(root, query))
		return false;

	/*
	 * Otherwise, we can throw away the targetlist, as well as any GROUP,
	 * DISTINCT, and ORDER BY clauses; none of those clauses will change
	 * a nonzero-rows result to zero rows or vice versa.  (Furthermore,
	 * since our parsetree representation of these clauses depends on the
	 * targetlist, we'd better throw them away if we drop the targetlist.)
	 */
	/* Delete ORDER BY and DISTINCT. */
	query->sortClause = NIL;
	query->distinctClause = NIL;

	/*
	 * HAVING is the only place that could still contain aggregates. We can
	 * delete targetlist if there is no havingQual.
	 */
	if (query->havingQual == NULL)
	{
		query->targetList = NULL;
		query->hasAggs = false;
	}

	/* If HAVING has no aggregates, demote it to WHERE. */
	else if (!checkExprHasAggs(query->havingQual))
	{
		query->jointree->quals = make_and_qual(query->jointree->quals,
												   query->havingQual);
		query->havingQual = NULL;
		query->hasAggs = false;
	}

	/* Delete GROUP BY if no aggregates. */
	if (!query->hasAggs)
		query->groupClause = NIL;

	return true;
}

/*
 * convert_EXISTS_sublink_to_join: try to convert an EXISTS SubLink to a join
 *
 * The API of this function is identical to convert_ANY_sublink_to_join's,
 * except that we also support the case where the caller has found NOT EXISTS,
 * so we need an additional input parameter "under_not".
 */
Node*
convert_EXISTS_sublink_to_join(PlannerInfo *root, SubLink *sublink,
							   bool under_not, Relids available_rels)
{
	JoinExpr   *result;
	Query	   *parse = root->parse;
	Query	   *subselect = (Query *) sublink->subselect;
	Node	   *whereClause;
	int			rtoffset;
	int			varno;
	Relids		clause_varnos;
	Relids		upper_varnos;
	Node		*limitqual = NULL;
	Node		*lnode;
	Node		*rnode;
	Node		*node;

	Assert(sublink->subLinkType == EXISTS_SUBLINK);

	Assert(IsA(subselect, Query));

	if (has_correlation_in_funcexpr_rte(subselect->rtable))
		return NULL;

	/*
	 * If deeply correlated, don't bother.
	 */
	if (IsSubqueryMultiLevelCorrelated(subselect))
		return NULL;

	/*
	* Don't remove the sublink if we cannot pull-up the subquery
	* later during pull_up_simple_subquery()
	*/
	if (!simplify_EXISTS_query(root, subselect))
		return NULL;

	/*
	 * 'LIMIT n' makes EXISTS false when n <= 0, and doesn't affect the
	 * outcome when n > 0.  Delete subquery's LIMIT and build (0 < n) expr to
	 * be ANDed into the parent qual.
	 */
	if (subselect->limitCount)
	{
		rnode = copyObject(subselect->limitCount);
		IncrementVarSublevelsUp(rnode, -1, 1);
		lnode = (Node *) makeConst(INT8OID, -1, sizeof(int64), Int64GetDatum(0),
								   false, true);
		limitqual = (Node *) make_op(NULL, list_make1(makeString("<")),
									 lnode, rnode, -1);
		subselect->limitCount = NULL;
	}

	/* CDB TODO: Set-returning function in tlist could return empty set. */
	if (expression_returns_set((Node *) subselect->targetList))
		ereport(ERROR, (errcode(ERRCODE_GP_FEATURE_NOT_YET),
						errmsg("Set-returning function in EXISTS subquery: not yet implemented")
						));

	/*
	 * Trivial EXISTS subquery can be eliminated altogether.  If subquery has
	 * aggregates without GROUP BY or HAVING, its result is exactly one row
	 * (assuming no errors), unless that row is discarded by LIMIT/OFFSET.
	 */
	if (subselect->hasAggs &&
		subselect->groupClause == NIL &&
		subselect->havingQual == NULL)
	{
		/*
		 * 'OFFSET m' falsifies EXISTS for m >= 1, and doesn't affect the
		 * outcome for m < 1, given that the subquery yields at most one row.
		 * Delete subquery's OFFSET and build (m < 1) expr to be anded with
		 * the current query's WHERE clause.
		 */
		if (subselect->limitOffset)
		{
			lnode = copyObject(subselect->limitOffset);
			IncrementVarSublevelsUp(lnode, -1, 1);
			rnode = (Node *) makeConst(INT8OID, -1, sizeof(int64), Int64GetDatum(1),
									   false, true);
			node = (Node *) make_op(NULL, list_make1(makeString("<")),
									lnode, rnode, -1);
			limitqual = make_and_qual(limitqual, node);
		}

		/* Replace trivial EXISTS(...) with TRUE if no LIMIT/OFFSET. */
		if (limitqual == NULL)
			limitqual = makeBoolConst(true, false);

		if (under_not)
			return (Node *) make_notclause((Expr *)limitqual);

		return limitqual;
	}

	/*
	 * If uncorrelated, the subquery will be executed only once.  Add LIMIT 1
	 * and let the SubLink remain unflattened.  It will become an InitPlan.
	 * (CDB TODO: Would it be better to go ahead and convert these to joins?)
	 */
	if (!contain_vars_of_level_or_above(sublink->subselect, 1))
	{
		subselect->limitCount = (Node *) makeConst(INT8OID, -1, sizeof(int64), Int64GetDatum(1),
												   false, true);
		node = make_and_qual(limitqual, (Node *) sublink);
		if (under_not)
			return (Node *) make_notclause((Expr *)node);
		return node;
	}

	/*
	 * Separate out the WHERE clause.  (We could theoretically also remove
	 * top-level plain JOIN/ON clauses, but it's probably not worth the
	 * trouble.)
	 */
	whereClause = subselect->jointree->quals;
	subselect->jointree->quals = NULL;

	/*
	 * The rest of the sub-select must not refer to any Vars of the parent
	 * query.  (Vars of higher levels should be okay, though.)
	 */
	if (contain_vars_of_level((Node *) subselect, 1))
		return NULL;

	/*
	 * On the other hand, the WHERE clause must contain some Vars of the
	 * parent query, else it's not gonna be a join.
	 */
	if (!contain_vars_of_level(whereClause, 1))
		return NULL;

	/*
	 * We don't risk optimizing if the WHERE clause is volatile, either.
	 */
	if (contain_volatile_functions(whereClause))
		return NULL;

	/*
	 * Prepare to pull up the sub-select into top range table.
	 *
	 * We rely here on the assumption that the outer query has no references
	 * to the inner (necessarily true). Therefore this is a lot easier than
	 * what pull_up_subqueries has to go through.
	 *
	 * In fact, it's even easier than what convert_ANY_sublink_to_join has
	 * to do.  The machinations of simplify_EXISTS_query ensured that there
	 * is nothing interesting in the subquery except an rtable and jointree,
	 * and even the jointree FromExpr no longer has quals.  So we can just
	 * append the rtable to our own and use the FromExpr in our jointree.
	 * But first, adjust all level-zero varnos in the subquery to account
	 * for the rtable merger.
	 */
	rtoffset = list_length(parse->rtable);
	OffsetVarNodes((Node *) subselect, rtoffset, 0);
	OffsetVarNodes(whereClause, rtoffset, 0);

	/*
	 * Upper-level vars in subquery will now be one level closer to their
	 * parent than before; in particular, anything that had been level 1
	 * becomes level zero.
	 */
	IncrementVarSublevelsUp((Node *) subselect, -1, 1);
	IncrementVarSublevelsUp(whereClause, -1, 1);

	/*
	 * Now that the WHERE clause is adjusted to match the parent query
	 * environment, we can easily identify all the level-zero rels it uses.
	 * The ones <= rtoffset belong to the upper query; the ones > rtoffset
	 * do not.
	 */
	clause_varnos = pull_varnos(whereClause);
	upper_varnos = NULL;
	while ((varno = bms_first_member(clause_varnos)) >= 0)
	{
		if (varno <= rtoffset)
			upper_varnos = bms_add_member(upper_varnos, varno);
	}
	bms_free(clause_varnos);
	Assert(!bms_is_empty(upper_varnos));

	/*
	 * Now that we've got the set of upper-level varnos, we can make the
	 * last check: only available_rels can be referenced.
	 */
	if (!bms_is_subset(upper_varnos, available_rels))
		return NULL;

	/* Now we can attach the modified subquery rtable to the parent */
	parse->rtable = list_concat(parse->rtable, subselect->rtable);


	/*
	 * And finally, build the JoinExpr node.
	 */
	result = makeNode(JoinExpr);
	result->jointype = under_not ? JOIN_ANTI : JOIN_SEMI;
	result->isNatural = false;
	result->larg = NULL;		/* caller must fill this in */
	/* flatten out the FromExpr node if it's useless */
	if (list_length(subselect->jointree->fromlist) == 1)
		result->rarg = (Node *) linitial(subselect->jointree->fromlist);
	else
		result->rarg = (Node *) subselect->jointree;
	result->quals = whereClause;
	result->alias = NULL;
	result->rtindex = 0;

	return (Node *) result;
}

/*
 * convert_EXISTS_to_ANY: try to convert EXISTS to a hashable ANY sublink
 *
 * The subselect is expected to be a fresh copy that we can munge up,
 * and to have been successfully passed through simplify_EXISTS_query.
 *
 * On success, the modified subselect is returned, and we store a suitable
 * upper-level test expression at *testexpr, plus a list of the subselect's
 * output Params at *paramIds.  (The test expression is already Param-ified
 * and hence need not go through convert_testexpr, which is why we have to
 * deal with the Param IDs specially.)
 *
 * On failure, returns NULL.
 */
static Query *
convert_EXISTS_to_ANY(PlannerInfo *root, Query *subselect,
					  Node **testexpr, List **paramIds)
{
	Node	   *whereClause;
	List	   *leftargs,
	*rightargs,
	*opids,
	*newWhere,
	*tlist,
	*testlist,
	*paramids;
	ListCell   *lc,
	*rc,
	*oc;
	AttrNumber	resno;
	
	/*
	 * Query must not require a targetlist, since we have to insert a new one.
	 * Caller should have dealt with the case already.
	 */
	Assert(subselect->targetList == NIL);
	
	/*
	 * Separate out the WHERE clause.  (We could theoretically also remove
	 * top-level plain JOIN/ON clauses, but it's probably not worth the
	 * trouble.)
	 */
	whereClause = subselect->jointree->quals;
	subselect->jointree->quals = NULL;
	
	/*
	 * The rest of the sub-select must not refer to any Vars of the parent
	 * query.  (Vars of higher levels should be okay, though.)
	 *
	 * Note: we need not check for Aggs separately because we know the
	 * sub-select is as yet unoptimized; any uplevel Agg must therefore
	 * contain an uplevel Var reference.  This is not the case below ...
	 */
	if (contain_vars_of_level((Node *) subselect, 1))
		return NULL;
	
	/*
	 * We don't risk optimizing if the WHERE clause is volatile, either.
	 */
	if (contain_volatile_functions(whereClause))
		return NULL;
	
	/*
	 * Clean up the WHERE clause by doing const-simplification etc on it.
	 * Aside from simplifying the processing we're about to do, this is
	 * important for being able to pull chunks of the WHERE clause up into
	 * the parent query.  Since we are invoked partway through the parent's
	 * preprocess_expression() work, earlier steps of preprocess_expression()
	 * wouldn't get applied to the pulled-up stuff unless we do them here.
	 * For the parts of the WHERE clause that get put back into the child
	 * query, this work is partially duplicative, but it shouldn't hurt.
	 *
	 * Note: we do not run flatten_join_alias_vars.  This is OK because
	 * any parent aliases were flattened already, and we're not going to
	 * pull any child Vars (of any description) into the parent.
	 *
	 * Note: passing the parent's root to eval_const_expressions is technically
	 * wrong, but we can get away with it since only the boundParams (if any)
	 * are used, and those would be the same in a subroot.
	 */
	whereClause = eval_const_expressions(root, whereClause);
	whereClause = (Node *) canonicalize_qual((Expr *) whereClause);
	whereClause = (Node *) make_ands_implicit((Expr *) whereClause);
	
	/*
	 * We now have a flattened implicit-AND list of clauses, which we
	 * try to break apart into "outervar = innervar" hash clauses.
	 * Anything that can't be broken apart just goes back into the
	 * newWhere list.  Note that we aren't trying hard yet to ensure
	 * that we have only outer or only inner on each side; we'll check
	 * that if we get to the end.
	 */
	leftargs = rightargs = opids = newWhere = NIL;
	foreach(lc, (List *) whereClause)
	{
		OpExpr	   *expr = (OpExpr *) lfirst(lc);
		
		if (IsA(expr, OpExpr) &&
			hash_ok_operator(expr))
		{
			Node   *leftarg = (Node *) linitial(expr->args);
			Node   *rightarg = (Node *) lsecond(expr->args);
			
			if (contain_vars_of_level(leftarg, 1))
			{
				leftargs = lappend(leftargs, leftarg);
				rightargs = lappend(rightargs, rightarg);
				opids = lappend_oid(opids, expr->opno);
				continue;
			}
			if (contain_vars_of_level(rightarg, 1))
			{
				/*
				 * We must commute the clause to put the outer var on the
				 * left, because the hashing code in nodeSubplan.c expects
				 * that.  This probably shouldn't ever fail, since hashable
				 * operators ought to have commutators, but be paranoid.
				 */
				expr->opno = get_commutator(expr->opno);
				if (OidIsValid(expr->opno) && hash_ok_operator(expr))
				{
					leftargs = lappend(leftargs, rightarg);
					rightargs = lappend(rightargs, leftarg);
					opids = lappend_oid(opids, expr->opno);
					continue;
				}
				/* If no commutator, no chance to optimize the WHERE clause */
				return NULL;
			}
		}
		/* Couldn't handle it as a hash clause */
		newWhere = lappend(newWhere, expr);
	}
	
	/*
	 * If we didn't find anything we could convert, fail.
	 */
	if (leftargs == NIL)
		return NULL;
	
	/*
	 * There mustn't be any parent Vars or Aggs in the stuff that we intend to
	 * put back into the child query.  Note: you might think we don't need to
	 * check for Aggs separately, because an uplevel Agg must contain an
	 * uplevel Var in its argument.  But it is possible that the uplevel Var
	 * got optimized away by eval_const_expressions.  Consider
	 *
	 * SUM(CASE WHEN false THEN uplevelvar ELSE 0 END)
	 */
	if (contain_vars_of_level((Node *) newWhere, 1) ||
		contain_vars_of_level((Node *) rightargs, 1))
		return NULL;
	if (root->parse->hasAggs &&
		(contain_aggs_of_level((Node *) newWhere, 1) ||
		 contain_aggs_of_level((Node *) rightargs, 1)))
		return NULL;
	
	/*
	 * And there can't be any child Vars in the stuff we intend to pull up.
	 * (Note: we'd need to check for child Aggs too, except we know the
	 * child has no aggs at all because of simplify_EXISTS_query's check.)
	 */
	if (contain_vars_of_level((Node *) leftargs, 0))
		return NULL;
	
	/*
	 * Also reject sublinks in the stuff we intend to pull up.  (It might be
	 * possible to support this, but doesn't seem worth the complication.)
	 */
	if (contain_subplans((Node *) leftargs))
		return NULL;
	
	/*
	 * Okay, adjust the sublevelsup in the stuff we're pulling up.
	 */
	IncrementVarSublevelsUp((Node *) leftargs, -1, 1);
	
	/*
	 * Put back any child-level-only WHERE clauses.
	 */
	if (newWhere)
		subselect->jointree->quals = (Node *) make_ands_explicit(newWhere);
	
	/*
	 * Build a new targetlist for the child that emits the expressions
	 * we need.  Concurrently, build a testexpr for the parent using
	 * Params to reference the child outputs.  (Since we generate Params
	 * directly here, there will be no need to convert the testexpr in
	 * build_subplan.)
	 */
	tlist = testlist = paramids = NIL;
	resno = 1;
	/* there's no "for3" so we have to chase one of the lists manually */
	oc = list_head(opids);
	forboth(lc, leftargs, rc, rightargs)
	{
		Node	   *leftarg = (Node *) lfirst(lc);
		Node	   *rightarg = (Node *) lfirst(rc);
		Oid			opid = lfirst_oid(oc);
		Param	   *param;
		
		oc = lnext(oc);
		param = generate_new_param(root,
								   exprType(rightarg),
								   exprTypmod(rightarg));
		tlist = lappend(tlist,
						makeTargetEntry((Expr *) rightarg,
										resno++,
										NULL,
										false));
		testlist = lappend(testlist,
						   make_opclause(opid, BOOLOID, false,
										 (Expr *) leftarg, (Expr *) param));
		paramids = lappend_int(paramids, param->paramid);
	}
	
	/* Put everything where it should go, and we're done */
	subselect->targetList = tlist;
	*testexpr = (Node *) make_ands_explicit(testlist);
	*paramIds = paramids;
	
	return subselect;
}

/*
 * Replace correlation vars (uplevel vars) with Params.
 *
 * Uplevel aggregates are replaced, too.
 *
 * Note: it is critical that this runs immediately after SS_process_sublinks.
 * Since we do not recurse into the arguments of uplevel aggregates, they will
 * get copied to the appropriate subplan args list in the parent query with
 * uplevel vars not replaced by Params, but only adjusted in level (see
 * replace_outer_agg).	That's exactly what we want for the vars of the parent
 * level --- but if an aggregate's argument contains any further-up variables,
 * they have to be replaced with Params in their turn.	That will happen when
 * the parent level runs SS_replace_correlation_vars.  Therefore it must do
 * so after expanding its sublinks to subplans.  And we don't want any steps
 * in between, else those steps would never get applied to the aggregate
 * argument expressions, either in the parent or the child level.
 *
 * Another fairly tricky thing going on here is the handling of SubLinks in
 * the arguments of uplevel aggregates.  Those are not touched inside the
 * intermediate query level, either.  Instead, SS_process_sublinks recurses
 * on them after copying the Aggref expression into the parent plan level
 * (this is actually taken care of in make_subplan).
 */
Node *
SS_replace_correlation_vars(PlannerInfo *root, Node *expr)
{
	/* No setup needed for tree walk, so away we go */
	return replace_correlation_vars_mutator(expr, root);
}

static Node *
replace_correlation_vars_mutator(Node *node, PlannerInfo *root)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Var))
	{
		if (((Var *) node)->varlevelsup > 0)
			return (Node *) replace_outer_var(root, (Var *) node);
	}
	if (IsA(node, Aggref))
	{
		if (((Aggref *) node)->agglevelsup > 0)
			return (Node *) replace_outer_agg(root, (Aggref *) node);
	}
	return expression_tree_mutator(node,
								   replace_correlation_vars_mutator,
								   (void *) root);
}

/*
 * Expand SubLinks to SubPlans in the given expression.
 *
 * The isQual argument tells whether or not this expression is a WHERE/HAVING
 * qualifier expression.  If it is, any sublinks appearing at top level need
 * not distinguish FALSE from UNKNOWN return values.
 */
Node *
SS_process_sublinks(PlannerInfo *root, Node *expr, bool isQual)
{
	process_sublinks_context context;

	context.root = root;
	context.isTopQual = isQual;
	return process_sublinks_mutator(expr, &context);
}

static Node *
process_sublinks_mutator(Node *node, process_sublinks_context *context)
{
	process_sublinks_context locContext;

	locContext.root = context->root;

	if (node == NULL)
		return NULL;
	if (IsA(node, SubLink))
	{
		SubLink    *sublink = (SubLink *) node;
		Node	   *testexpr;

		/*
		 * First, recursively process the lefthand-side expressions, if any.
		 * They're not top-level anymore.
		 */
		locContext.isTopQual = false;
		testexpr = process_sublinks_mutator(sublink->testexpr, &locContext);

		/*
		 * Now build the SubPlan node and make the expr to return.
		 */
		return make_subplan(context->root,
							(Query *) sublink->subselect,
							sublink->subLinkType,
							testexpr,
							context->isTopQual);
	}

	/*
	 * Don't recurse into the arguments of an outer aggregate here.
	 * Any SubLinks in the arguments have to be dealt with at the outer
	 * query level; they'll be handled when make_subplan collects the
	 * Aggref into the arguments to be passed down to the current subplan.
	 */
	if (IsA(node, Aggref))
	{
		if (((Aggref *) node)->agglevelsup > 0)
			return node;
	}

	/*
	 * We should never see a SubPlan expression in the input (since this is
	 * the very routine that creates 'em to begin with).  We shouldn't find
	 * ourselves invoked directly on a Query, either.
	 */
	Assert(!IsA(node, SubPlan));
	Assert(!IsA(node, AlternativeSubPlan));
	Assert(!IsA(node, Query));

	/*
	 * Because make_subplan() could return an AND or OR clause, we have to
	 * take steps to preserve AND/OR flatness of a qual.  We assume the input
	 * has been AND/OR flattened and so we need no recursion here.
	 *
	 * (Due to the coding here, we will not get called on the List subnodes of
	 * an AND; and the input is *not* yet in implicit-AND format.  So no check
	 * is needed for a bare List.)
	 *
	 * Anywhere within the top-level AND/OR clause structure, we can tell
	 * make_subplan() that NULL and FALSE are interchangeable.  So isTopQual
	 * propagates down in both cases.  (Note that this is unlike the meaning
	 * of "top level qual" used in most other places in Postgres.)
	 */
	if (and_clause(node))
	{
		List	   *newargs = NIL;
		ListCell   *l;

		/* Still at qual top-level */
		locContext.isTopQual = context->isTopQual;

		foreach(l, ((BoolExpr *) node)->args)
		{
			Node	   *newarg;

			newarg = process_sublinks_mutator(lfirst(l), &locContext);
			if (and_clause(newarg))
				newargs = list_concat(newargs, ((BoolExpr *) newarg)->args);
			else
				newargs = lappend(newargs, newarg);
		}
		return (Node *) make_andclause(newargs);
	}

	if (or_clause(node))
	{
		List	   *newargs = NIL;
		ListCell   *l;

		/* Still at qual top-level */
		locContext.isTopQual = context->isTopQual;

		foreach(l, ((BoolExpr *) node)->args)
		{
			Node	   *newarg;

			newarg = process_sublinks_mutator(lfirst(l), &locContext);
			if (or_clause(newarg))
				newargs = list_concat(newargs, ((BoolExpr *) newarg)->args);
			else
				newargs = lappend(newargs, newarg);
		}
		return (Node *) make_orclause(newargs);
	}

	/*
	 * If we recurse down through anything other than an AND or OR node,
	 * we are definitely not at top qual level anymore.
	 */
	locContext.isTopQual = false;

	return expression_tree_mutator(node,
								   process_sublinks_mutator,
								   (void *) &locContext);
}

/*
 * SS_finalize_plan - do final sublink processing for a completed Plan.
 *
 * This recursively computes the extParam and allParam sets for every Plan
 * node in the given plan tree.  It also optionally attaches any previously
 * generated InitPlans to the top plan node.  (Any InitPlans should already
 * have been put through SS_finalize_plan.)
 *
 * Input:
 * 	root - PlannerInfo structure that is necessary for walking the tree
 * 	rtable - list of rangetable entries to look at for relids
 * 	attach_initplans - attach all initplans to the top plan node from root
 * Output:
 * 	plan->extParam and plan->allParam - attach params to top of the plan
 */
void
SS_finalize_plan(PlannerInfo *root, Plan *plan, bool attach_initplans)
{
	Bitmapset  *valid_params,
			   *initExtParam,
			   *initSetParam;
	Cost		initplan_cost;
	int			paramid;
	ListCell   *l;

	/*
	 * Examine any initPlans to determine the set of external params they
	 * reference, the set of output params they supply, and their total cost.
	 * We'll use at least some of this info below.  (Note we are assuming that
	 * finalize_plan doesn't touch the initPlans.)
	 *
	 * In the case where attach_initplans is false, we are assuming that the
	 * existing initPlans are siblings that might supply params needed by the
	 * current plan.
	 */
	initExtParam = initSetParam = NULL;
	initplan_cost = 0;
	foreach(l, root->init_plans)
	{
		SubPlan    *initsubplan = (SubPlan *) lfirst(l);
		Plan	   *initplan = planner_subplan_get_plan(root, initsubplan);
		ListCell   *l2;

		initExtParam = bms_add_members(initExtParam, initplan->extParam);
		foreach(l2, initsubplan->setParam)
		{
			initSetParam = bms_add_member(initSetParam, lfirst_int(l2));
		}
		initplan_cost += initsubplan->startup_cost + initsubplan->per_call_cost;
	}

	/*
	 * Now determine the set of params that are validly referenceable in this
	 * query level; to wit, those available from outer query levels plus the
	 * output parameters of any initPlans.  (We do not include output
	 * parameters of regular subplans.  Those should only appear within the
	 * testexpr of SubPlan nodes, and are taken care of locally within
	 * finalize_primnode.)
	 *
	 * Note: this is a bit overly generous since some parameters of upper
	 * query levels might belong to query subtrees that don't include this
	 * query.  However, valid_params is only a debugging crosscheck, so it
	 * doesn't seem worth expending lots of cycles to try to be exact.
	 */
	valid_params = bms_copy(initSetParam);
	paramid = 0;
	foreach(l, root->glob->paramlist)
	{
		PlannerParamItem *pitem = (PlannerParamItem *) lfirst(l);

		if (pitem->abslevel < root->query_level)
		{
			/* valid outer-level parameter */
			valid_params = bms_add_member(valid_params, paramid);
		}

		paramid++;
	}
	/* Also include the recursion working table, if any */
	if (root->wt_param_id >= 0)
		valid_params = bms_add_member(valid_params, root->wt_param_id);

	/*
	 * Now recurse through plan tree.
	 */
	(void) finalize_plan(root, plan, valid_params);

	bms_free(valid_params);

	/*
	 * Finally, attach any initPlans to the topmost plan node, and add their
	 * extParams to the topmost node's, too.  However, any setParams of the
	 * initPlans should not be present in the topmost node's extParams, only
	 * in its allParams.  (As of PG 8.1, it's possible that some initPlans
	 * have extParams that are setParams of other initPlans, so we have to
	 * take care of this situation explicitly.)
	 *
	 * We also add the eval cost of each initPlan to the startup cost of the
	 * top node.  This is a conservative overestimate, since in fact each
	 * initPlan might be executed later than plan startup, or even not at all.
	 */
	if (attach_initplans)
	{
		/*
		 * If the topmost plan is a Motion, attach the InitPlan to the node
		 * below it, instead. The executor machinery that executes InitPlans
		 * in the QD node, and sends the resulting exec parameters to the QE
		 * nodes, gets confused if the InitPlan is attached to the Motion
		 * node, and fails to deliver the exec parameter value to where it's
		 * needed. I'm not sure why that fails, but historically the InitPlans
		 * have always been attached to the node below the Motion, so let's
		 * just keep that behavior for now.
		 */
		if (IsA(plan, Motion))
			plan = plan->lefttree;

		Insist(!plan->initPlan);
		plan->initPlan = root->init_plans;
		root->init_plans = NIL;		/* make sure they're not attached twice */

		/* allParam must include all these params */
		plan->allParam = bms_add_members(plan->allParam, initExtParam);
		plan->allParam = bms_add_members(plan->allParam, initSetParam);
		/* extParam must include any child extParam */
		plan->extParam = bms_add_members(plan->extParam, initExtParam);
		/* but extParam shouldn't include any setParams */
		plan->extParam = bms_del_members(plan->extParam, initSetParam);
		/* ensure extParam is exactly NULL if it's empty */
		if (bms_is_empty(plan->extParam))
			plan->extParam = NULL;

		plan->startup_cost += initplan_cost;
		plan->total_cost += initplan_cost;
	}
}

/*
 * Recursive processing of all nodes in the plan tree
 *
 * The return value is the computed allParam set for the given Plan node.
 * This is just an internal notational convenience.
 */
static Bitmapset *
finalize_plan(PlannerInfo *root, Plan *plan, Bitmapset *valid_params)
{
	finalize_primnode_context context;

	if (plan == NULL)
		return NULL;

	context.root = root;
	context.paramids = NULL;	/* initialize set to empty */

	/*
	 * When we call finalize_primnode, context.paramids sets are automatically
	 * merged together.  But when recursing to self, we have to do it the hard
	 * way.  We want the paramids set to include params in subplans as well as
	 * at this level.
	 */

	/* Find params in targetlist and qual */
	finalize_primnode((Node *) plan->targetlist, &context);
	finalize_primnode((Node *) plan->qual, &context);

	/* Check additional node-type-specific fields */
	switch (nodeTag(plan))
	{
		case T_Result:
			finalize_primnode(((Result *) plan)->resconstantqual,
							  &context);
			break;

		case T_IndexScan:
			finalize_primnode((Node *) ((IndexScan *) plan)->indexqual,
							  &context);

			/*
			 * we need not look at indexqualorig, since it will have the same
			 * param references as indexqual.
			 */
			break;

		case T_BitmapIndexScan:
			finalize_primnode((Node *) ((BitmapIndexScan *) plan)->indexqual,
							  &context);

			/*
			 * we need not look at indexqualorig, since it will have the same
			 * param references as indexqual.
			 */
			break;

		case T_BitmapHeapScan:
			finalize_primnode((Node *) ((BitmapHeapScan *) plan)->bitmapqualorig,
							  &context);
			break;

		case T_BitmapAppendOnlyScan:
			finalize_primnode((Node *) ((BitmapAppendOnlyScan *) plan)->bitmapqualorig,
							  &context);
			break;

		case T_BitmapTableScan:
			finalize_primnode((Node *) ((BitmapTableScan *) plan)->bitmapqualorig,
							  &context);
			break;

		case T_TidScan:
			finalize_primnode((Node *) ((TidScan *) plan)->tidquals,
							  &context);
			break;

		case T_SubqueryScan:

			/*
			 * In a SubqueryScan, SS_finalize_plan has already been run on the
			 * subplan by the inner invocation of subquery_planner, so there's
			 * no need to do it again.  Instead, just pull out the subplan's
			 * extParams list, which represents the params it needs from my
			 * level and higher levels.
			 */
			context.paramids = bms_add_members(context.paramids,
								 ((SubqueryScan *) plan)->subplan->extParam);
			break;

		case T_TableFunctionScan:
			{
				RangeTblEntry *rte;

				rte = rt_fetch(((TableFunctionScan *) plan)->scan.scanrelid,
							   root->parse->rtable);
				Assert(rte->rtekind == RTE_TABLEFUNCTION);
				finalize_primnode(rte->funcexpr, &context);
			}
			/* TableFunctionScan's lefttree is like SubqueryScan's subplan. */
			context.paramids = bms_add_members(context.paramids,
								 plan->lefttree->extParam);
			break;

		case T_FunctionScan:
			finalize_primnode(((FunctionScan *) plan)->funcexpr,
							  &context);
			break;

		case T_ValuesScan:
			finalize_primnode((Node *) ((ValuesScan *) plan)->values_lists,
							  &context);
			break;

		case T_CteScan:
			{
				/*
				 * You might think we should add the node's cteParam to
				 * paramids, but we shouldn't because that param is just a
				 * linkage mechanism for multiple CteScan nodes for the same
				 * CTE; it is never used for changed-param signaling.  What
				 * we have to do instead is to find the referenced CTE plan
				 * and incorporate its external paramids, so that the correct
				 * things will happen if the CTE references outer-level
				 * variables.  See test cases for bug #4902.
				 */
				int		plan_id = ((CteScan *) plan)->ctePlanId;
				Plan   *cteplan;

				/* so, do this ... */
				if (plan_id < 1 || plan_id > list_length(root->glob->subplans))
					elog(ERROR, "could not find plan for CteScan referencing plan ID %d",
						 plan_id);
				cteplan = (Plan *) list_nth(root->glob->subplans, plan_id - 1);
				context.paramids =
					bms_add_members(context.paramids, cteplan->extParam);

#ifdef NOT_USED
				/* ... but not this */
				context.paramids =
					bms_add_member(context.paramids,
								   ((CteScan *) plan)->cteParam);
#endif
			}
			break;

		case T_WorkTableScan:
			context.paramids =
				bms_add_member(context.paramids,
							   ((WorkTableScan *) plan)->wtParam);
			break;

		case T_Append:
			{
				ListCell   *l;

				foreach(l, ((Append *) plan)->appendplans)
				{
					context.paramids =
						bms_add_members(context.paramids,
										finalize_plan(root,
													  (Plan *) lfirst(l),
													  valid_params));
				}
			}
			break;

		case T_BitmapAnd:
			{
				ListCell   *l;

				foreach(l, ((BitmapAnd *) plan)->bitmapplans)
				{
					context.paramids =
						bms_add_members(context.paramids,
										finalize_plan(root,
													  (Plan *) lfirst(l),
													  valid_params));
				}
			}
			break;

		case T_BitmapOr:
			{
				ListCell   *l;

				foreach(l, ((BitmapOr *) plan)->bitmapplans)
				{
					context.paramids =
						bms_add_members(context.paramids,
										finalize_plan(root,
													  (Plan *) lfirst(l),
													  valid_params));
				}
			}
			break;

		case T_NestLoop:
			finalize_primnode((Node *) ((Join *) plan)->joinqual,
							  &context);
			break;

		case T_MergeJoin:
			finalize_primnode((Node *) ((Join *) plan)->joinqual,
							  &context);
			finalize_primnode((Node *) ((MergeJoin *) plan)->mergeclauses,
							  &context);
			break;

		case T_HashJoin:
			finalize_primnode((Node *) ((Join *) plan)->joinqual,
							  &context);
			finalize_primnode((Node *) ((HashJoin *) plan)->hashclauses,
							  &context);
			finalize_primnode((Node *) ((HashJoin *) plan)->hashqualclauses,
							  &context);
			break;

		case T_Motion:

			finalize_primnode((Node *) ((Motion *) plan)->hashExpr,
							  &context);
			break;

		case T_Limit:
			finalize_primnode(((Limit *) plan)->limitOffset,
							  &context);
			finalize_primnode(((Limit *) plan)->limitCount,
							  &context);
			break;

		case T_PartitionSelector:
			finalize_primnode((Node *) ((PartitionSelector *) plan)->levelEqExpressions,
							  &context);
			finalize_primnode((Node *) ((PartitionSelector *) plan)->levelExpressions,
							  &context);
			finalize_primnode(((PartitionSelector *) plan)->residualPredicate,
							  &context);	
			finalize_primnode(((PartitionSelector *) plan)->propagationExpression,
							  &context);
			finalize_primnode(((PartitionSelector *) plan)->printablePredicate,
							  &context);
			finalize_primnode((Node *) ((PartitionSelector *) plan)->partTabTargetlist,
							  &context);
			break;
			
		case T_RecursiveUnion:
		case T_Hash:
		case T_Agg:
		case T_WindowAgg:
		case T_SeqScan:
		case T_AppendOnlyScan:
		case T_AOCSScan: 
		case T_ExternalScan:
		case T_Material:
		case T_Sort:
		case T_ShareInputScan:
		case T_Unique:
		case T_SetOp:
		case T_Repeat:
			break;

		default:
			elog(ERROR, "unrecognized node type: %d",
				 (int) nodeTag(plan));
	}

	/* Process left and right child plans, if any */
	/*
	 * In a TableFunctionScan, the 'lefttree' is more like a SubQueryScan's
	 * subplan, and contains a plan that's already been finalized by the
	 * inner invocation of subquery_planner(). So skip that.
	 */
	if (!IsA(plan, TableFunctionScan))
		context.paramids = bms_add_members(context.paramids,
										   finalize_plan(root,
														 plan->lefttree,
														 valid_params));

	context.paramids = bms_add_members(context.paramids,
									   finalize_plan(root,
													 plan->righttree,
													 valid_params));

	/*
	 * RecursiveUnion *generates* its worktable param, so don't bubble that up
	 */
	if (IsA(plan, RecursiveUnion))
	{
		context.paramids = bms_del_member(context.paramids,
										  ((RecursiveUnion *) plan)->wtParam);
	}

	/* Now we have all the paramids */

	if (!bms_is_subset(context.paramids, valid_params))
		elog(ERROR, "plan should not reference subplan's variable");

	/*
	 * Note: by definition, extParam and allParam should have the same value
	 * in any plan node that doesn't have child initPlans.  We set them
	 * equal here, and later SS_finalize_plan will update them properly
	 * in node(s) that it attaches initPlans to.
	 *
	 * For speed at execution time, make sure extParam/allParam are actually
	 * NULL if they are empty sets.
	 */
	if (bms_is_empty(context.paramids))
	{
		plan->extParam = NULL;
		plan->allParam = NULL;
	}
	else
	{
		plan->extParam = context.paramids;
		plan->allParam = bms_copy(context.paramids);
	}

	return plan->allParam;
}

/*
 * finalize_primnode: add IDs of all PARAM_EXEC params appearing in the given
 * expression tree to the result set.
 */
static bool
finalize_primnode(Node *node, finalize_primnode_context *context)
{
	if (node == NULL)
		return false;
	if (IsA(node, Param))
	{
		if (((Param *) node)->paramkind == PARAM_EXEC)
		{
			int			paramid = ((Param *) node)->paramid;

			context->paramids = bms_add_member(context->paramids, paramid);
		}
		return false;			/* no more to do here */
	}
	if (IsA(node, SubPlan))
	{
		SubPlan    *subplan = (SubPlan *) node;
		Plan	   *plan = planner_subplan_get_plan(context->root, subplan);
		ListCell   *lc;
		Bitmapset  *subparamids;

		/* Recurse into the testexpr, but not into the Plan */
		finalize_primnode(subplan->testexpr, context);

		/*
		 * Remove any param IDs of output parameters of the subplan that were
		 * referenced in the testexpr.  These are not interesting for
		 * parameter change signaling since we always re-evaluate the subplan.
		 * Note that this wouldn't work too well if there might be uses of the
		 * same param IDs elsewhere in the plan, but that can't happen because
		 * generate_new_param never tries to merge params.
		 */
		foreach(lc, subplan->paramIds)
		{
			context->paramids = bms_del_member(context->paramids,
											   lfirst_int(lc));
		}

		/* Also examine args list */
		finalize_primnode((Node *) subplan->args, context);

		/*
		 * Add params needed by the subplan to paramids, but excluding those
		 * we will pass down to it.
		 */
		subparamids = bms_copy(plan->extParam);
		foreach(lc, subplan->parParam)
		{
			subparamids = bms_del_member(subparamids, lfirst_int(lc));
		}
		context->paramids = bms_join(context->paramids, subparamids);

		return false;			/* no more to do here */
	}
	return expression_tree_walker(node, finalize_primnode,
								  (void *) context);
}

/*
 * SS_make_initplan_from_plan - given a plan tree, make it an InitPlan
 *
 * The plan is expected to return a scalar value of the indicated type.
 * We build an EXPR_SUBLINK SubPlan node and put it into the initplan
 * list for the current query level.  A Param that represents the initplan's
 * output is returned.
 *
 * We assume the plan hasn't been put through SS_finalize_plan.
 *
 * We treat root->init_plans like the old PlannerInitPlan global here.
 */
Param *
SS_make_initplan_from_plan(PlannerInfo *root, Plan *plan,
						   Oid resulttype, int32 resulttypmod)
{
	SubPlan    *node;
	Param	   *prm;

	/*
	 * We must run SS_finalize_plan(), since that's normally done before a
	 * subplan gets put into the initplan list.  Tell it not to attach any
	 * pre-existing initplans to this one, since they are siblings not
	 * children of this initplan.  (This is something else that could perhaps
	 * be cleaner if we did extParam/allParam processing in setrefs.c instead
	 * of here?  See notes for materialize_finished_plan.)
	 */

	/*
	 * Build extParam/allParam sets for plan nodes.
	 */
	SS_finalize_plan(root, plan, false);

	/*
	 * Add the subplan and its rtable to the global lists.
	 */
	root->glob->subplans = lappend(root->glob->subplans,
								   plan);
	root->glob->subrtables = lappend(root->glob->subrtables,
									 root->parse->rtable);

	/*
	 * Create a SubPlan node and add it to the outer list of InitPlans.
	 * Note it has to appear after any other InitPlans it might depend on
	 * (see comments in ExecReScan).
	 */
	node = makeNode(SubPlan);
	node->subLinkType = EXPR_SUBLINK;
	get_first_col_type(plan, &node->firstColType, &node->firstColTypmod);
    node->qDispSliceId = 0;             /*CDB*/
	node->plan_id = list_length(root->glob->subplans);
	node->is_initplan = true;
	root->init_plans = lappend(root->init_plans, node);

	/*
	 * The node can't have any inputs (since it's an initplan), so the
	 * parParam and args lists remain empty.
	 */
	
	/* NB PostgreSQL calculates subplan cost here, but GPDB does it elsewhere. */

	/*
	 * Make a Param that will be the subplan's output.
	 */
	prm = generate_new_param(root, resulttype, resulttypmod);
	node->setParam = list_make1_int(prm->paramid);
	
	/* Label the subplan for EXPLAIN purposes */
	StringInfo buf = makeStringInfo();
	appendStringInfo(buf, "InitPlan %d (returns $%d)",
			node->plan_id, prm->paramid);
	node->plan_name = pstrdup(buf->data);
	pfree(buf->data);
	pfree(buf);
	buf = NULL;

	return prm;
}
