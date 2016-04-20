#include "postgres.h"
#include "optimizer/cost.h"
#include "optimizer/restrictinfo.h"
#include "hooks.h"
#include "utils.h"
#include "pathman.h"
#include "pickyappend.h"


set_join_pathlist_hook_type		set_join_pathlist_next = NULL;
set_rel_pathlist_hook_type		set_rel_pathlist_hook_next = NULL;


void
pathman_join_pathlist_hook(PlannerInfo *root,
						   RelOptInfo *joinrel,
						   RelOptInfo *outerrel,
						   RelOptInfo *innerrel,
						   JoinType jointype,
						   JoinPathExtraData *extra)
{
	JoinCostWorkspace	workspace;
	Path			   *outer,
					   *inner;
	Relids				inner_required;
	RangeTblEntry	   *inner_entry = root->simple_rte_array[innerrel->relid];
	PartRelationInfo   *inner_prel;
	NestPath		   *nest_path;
	List			   *pathkeys = NIL;
	List			   *joinrestrictclauses = extra->restrictlist;
	List			   *joinclauses,
					   *otherclauses;
	ListCell		   *lc;

	if (set_join_pathlist_next)
		set_join_pathlist_next(root, joinrel, outerrel,
							   innerrel, jointype, extra);

	if (jointype == JOIN_UNIQUE_OUTER ||
		jointype == JOIN_UNIQUE_INNER)
	{
		jointype = JOIN_INNER;
	}

	if (jointype == JOIN_FULL || !pg_pathman_enable_pickyappend)
		return;

	if (innerrel->reloptkind != RELOPT_BASEREL ||
		!inner_entry->inh ||
		!(inner_prel = get_pathman_relation_info(inner_entry->relid, NULL)))
	{
		return; /* Obviously not our case */
	}

	/* Extract join clauses which will separate partitions */
	if (IS_OUTER_JOIN(extra->sjinfo->jointype))
	{
		extract_actual_join_clauses(joinrestrictclauses,
									&joinclauses, &otherclauses);
	}
	else
	{
		/* We can treat all clauses alike for an inner join */
		joinclauses = extract_actual_clauses(joinrestrictclauses, false);
		otherclauses = NIL;
	}

	foreach (lc, innerrel->pathlist)
	{
		AppendPath *cur_inner_path = (AppendPath *) lfirst(lc);

		if (!IsA(cur_inner_path, AppendPath))
			continue;

		outer = outerrel->cheapest_total_path;

		inner_required = bms_union(PATH_REQ_OUTER((Path *) cur_inner_path),
								   bms_make_singleton(outerrel->relid));

		inner = create_pickyappend_path(root, cur_inner_path,
										get_appendrel_parampathinfo(innerrel,
																	inner_required),
										joinclauses);

		initial_cost_nestloop(root, &workspace, jointype,
							  outer, inner,
							  extra->sjinfo, &extra->semifactors);

		pathkeys = build_join_pathkeys(root, joinrel, jointype, outer->pathkeys);

		nest_path = create_nestloop_path(root, joinrel, jointype, &workspace,
										 extra->sjinfo, &extra->semifactors,
										 outer, inner, extra->restrictlist,
										 pathkeys,
										 calc_nestloop_required_outer(outer, inner));

		add_path(joinrel, (Path *) nest_path);
	}
}

/*
 * Main hook. All the magic goes here
 */
void
pathman_rel_pathlist_hook(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	PartRelationInfo *prel = NULL;
	RelOptInfo **new_rel_array;
	RangeTblEntry **new_rte_array;
	int len;
	bool found;
	int first_child_relid = 0;

	if (!pg_pathman_enable)
		return;

	/* This works only for SELECT queries */
	if (root->parse->commandType != CMD_SELECT || !inheritance_disabled)
		return;

	/* Lookup partitioning information for parent relation */
	prel = get_pathman_relation_info(rte->relid, &found);

	if (prel != NULL && found)
	{
		ListCell   *lc;
		int			i;
		Oid		   *dsm_arr;
		List	   *ranges,
				   *wrappers;
		PathKey	   *pathkeyAsc = NULL,
				   *pathkeyDesc = NULL;

		if (prel->parttype == PT_RANGE)
		{
			/*
			 * Get pathkeys for ascending and descending sort by partition
			 * column
			 */
			List		   *pathkeys;
			Var			   *var;
			Oid				vartypeid,
							varcollid;
			int32			type_mod;
			TypeCacheEntry *tce;

			/* Make Var from patition column */
			get_rte_attribute_type(rte, prel->attnum,
								   &vartypeid, &type_mod, &varcollid);
			var = makeVar(rti, prel->attnum, vartypeid, type_mod, varcollid, 0);
			var->location = -1;

			/* Determine operator type */
			tce = lookup_type_cache(var->vartype, TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);

			/* Make pathkeys */
			pathkeys = build_expression_pathkey(root, (Expr *)var, NULL,
												tce->lt_opr, NULL, false);
			if (pathkeys)
				pathkeyAsc = (PathKey *) linitial(pathkeys);
			pathkeys = build_expression_pathkey(root, (Expr *)var, NULL,
												tce->gt_opr, NULL, false);
			if (pathkeys)
				pathkeyDesc = (PathKey *) linitial(pathkeys);
		}

		rte->inh = true;
		dsm_arr = (Oid *) dsm_array_get_pointer(&prel->children);
		ranges = list_make1_int(make_irange(0, prel->children_count - 1, false));

		/* Make wrappers over restrictions and collect final rangeset */
		wrappers = NIL;
		foreach(lc, rel->baserestrictinfo)
		{
			WrapperNode *wrap;

			RestrictInfo *rinfo = (RestrictInfo*) lfirst(lc);

			wrap = walk_expr_tree(NULL, rinfo->clause, prel);
			wrappers = lappend(wrappers, wrap);
			ranges = irange_list_intersect(ranges, wrap->rangeset);
		}

		/*
		 * Expand simple_rte_array and simple_rel_array
		 */

		if (ranges)
		{
			len = irange_list_length(ranges);

			/* Expand simple_rel_array and simple_rte_array */
			new_rel_array = (RelOptInfo **)
				palloc0((root->simple_rel_array_size + len) * sizeof(RelOptInfo *));

			/* simple_rte_array is an array equivalent of the rtable list */
			new_rte_array = (RangeTblEntry **)
				palloc0((root->simple_rel_array_size + len) * sizeof(RangeTblEntry *));

			/* Copy relations to the new arrays */
	        for (i = 0; i < root->simple_rel_array_size; i++)
	        {
	                new_rel_array[i] = root->simple_rel_array[i];
	                new_rte_array[i] = root->simple_rte_array[i];
	        }

			/* Free old arrays */
			pfree(root->simple_rel_array);
			pfree(root->simple_rte_array);

			root->simple_rel_array_size += len;
			root->simple_rel_array = new_rel_array;
			root->simple_rte_array = new_rte_array;
		}

		/*
		 * Iterate all indexes in rangeset and append corresponding child
		 * relations.
		 */
		foreach(lc, ranges)
		{
			IndexRange	irange = lfirst_irange(lc);
			Oid			childOid;

			for (i = irange_lower(irange); i <= irange_upper(irange); i++)
			{
				int idx;

				childOid = dsm_arr[i];
				idx = append_child_relation(root, rel, rti, rte, i, childOid, wrappers);

				if (!first_child_relid)
					first_child_relid = idx;
			}
		}

		/* Clear old path list */
		list_free(rel->pathlist);

		rel->pathlist = NIL;
		set_append_rel_pathlist(root, rel, rti, rte, pathkeyAsc, pathkeyDesc);
		set_append_rel_size(root, rel, rti, rte);

		foreach (lc, rel->pathlist)
		{
			AppendPath	   *cur_path = (AppendPath *) lfirst(lc);
			Relids			inner_required = PATH_REQ_OUTER((Path *) cur_path);
			ParamPathInfo  *ppi = get_appendrel_parampathinfo(rel, inner_required);
			Path		   *inner_path;
			ListCell	   *subpath_cell;
			List		   *picky_quals = NIL;

			if (!IsA(cur_path, AppendPath) ||
				rel->has_eclass_joins ||
				rel->joininfo)
			{
				continue;
			}

			foreach (subpath_cell, cur_path->subpaths)
			{
				Path			   *subpath = (Path *) lfirst(subpath_cell);
				RelOptInfo		   *child_rel = subpath->parent;
				List			   *quals;
				ListCell		   *qual_cell;
				ReplaceVarsContext	repl_var_cxt;

				repl_var_cxt.child = subpath->parent;
				repl_var_cxt.parent = rel;
				repl_var_cxt.sublevels_up = 0;

				quals = extract_actual_clauses(child_rel->baserestrictinfo, false);

				/* Do not proceed if there's a rel containing quals without params */
				if (!clause_contains_params((Node *) quals))
				{
					picky_quals = NIL; /* skip this path */
					break;
				}

				/* Replace child Vars with a parent rel's Var */
				quals = (List *) replace_child_vars_with_parent_var((Node *) quals,
																	&repl_var_cxt);

				/* Combine unique 'picky' quals */
				foreach (qual_cell, quals)
					picky_quals = list_append_unique(picky_quals,
													 (Node *) lfirst(qual_cell));
			}

			/*
			 * Dismiss PickyAppend if there
			 * are no parameterized quals
			 */
			if (picky_quals == NIL)
				continue;

			inner_path = create_pickyappend_path(root, cur_path,
												 ppi, picky_quals);

			add_path(rel, inner_path);
		}
	}

	/* Invoke original hook if needed */
	if (set_rel_pathlist_hook_next != NULL)
		set_rel_pathlist_hook_next(root, rel, rti, rte);
}

