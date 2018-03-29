/*
 * Copyright (c) 2016 Shanghai Jiao Tong University.
 *     All rights reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS
 *  IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 *  express or implied.  See the License for the specific language
 *  governing permissions and limitations under the License.
 *
 * For more about this software visit:
 *
 *      http://ipads.se.sjtu.edu.cn/projects/wukong
 *
 */

#pragma once

#include <boost/unordered_set.hpp>
#include <boost/unordered_map.hpp>
#include <algorithm>//sort
#include <regex>

#include "config.hpp"
#include "type.hpp"
#include "coder.hpp"
#include "adaptor.hpp"
#include "dgraph.hpp"
#include "query.hpp"

#include "mymath.hpp"
#include "timer.hpp"

using namespace std;

#define BUSY_POLLING_THRESHOLD 10000000 // busy polling task queue 10s
#define MIN_SNOOZE_TIME 10 // MIX snooze time
#define MAX_SNOOZE_TIME 80 // MAX snooze time

// The map is used to colloect the replies of sub-queries in fork-join execution
class Reply_Map {
private:

    struct Item {
        int count;
        SPARQLQuery parent_request;
        SPARQLQuery merged_reply;
    };

    boost::unordered_map<int, Item> internal_item_map;

public:
    void put_parent_request(SPARQLQuery &r, int cnt) {
        Item data;
        data.count = cnt;
        data.parent_request = r;
        if (r.is_optional() && r.optional_dispatched)
            data.merged_reply.result = r.result;

        internal_item_map[r.id] = data;
    }

    void put_reply(SPARQLQuery &r) {
        int pid = r.pid;
        Item &data = internal_item_map[pid];

        SPARQLQuery::Result &data_result = data.merged_reply.result;
        SPARQLQuery::Result &r_result = r.result;
        data.count--;

        if (data.parent_request.is_union())
            data_result.merge_union(r_result);
        else if (data.parent_request.is_optional()
                 && data.parent_request.optional_dispatched)
            data_result.merge_optional(r_result);
        else
            data_result.append_result(r_result);
    }

    bool is_ready(int pid) {
        return internal_item_map[pid].count == 0;
    }

    SPARQLQuery get_merged_reply(int pid) {
        SPARQLQuery r = internal_item_map[pid].parent_request;
        SPARQLQuery &merged_reply = internal_item_map[pid].merged_reply;

        r.result.col_num = merged_reply.result.col_num;
        r.result.blind = merged_reply.result.blind;
        r.result.row_num = merged_reply.result.row_num;
        r.result.attr_col_num = merged_reply.result.attr_col_num;
        r.result.v2c_map = merged_reply.result.v2c_map;

        r.result.result_table.swap(merged_reply.result.result_table);
        r.result.attr_res_table.swap(r.result.attr_res_table);
        internal_item_map.erase(pid);
        return r;
    }
};


typedef pair<int64_t, int64_t> int64_pair;

int64_t hash_pair(const int64_pair &x) {
    int64_t r = x.first;
    r = r << 32;
    r += x.second;
    return hash<int64_t>()(r);
}

// defined as constexpr due to switch-case
constexpr int const_pair(int t1, int t2) { return ((t1 << 4) | t2); }


// a vector of pointers of all local engines
class Engine;
std::vector<Engine *> engines;


class Engine {
private:
    class Message {
    public:
        int sid;
        int tid;
        Bundle bundle;

        Message(int sid, int tid, Bundle &bundle)
            : sid(sid), tid(tid), bundle(bundle) { }
    };

    pthread_spinlock_t recv_lock;
    std::vector<SPARQLQuery> msg_fast_path;
    std::vector<SPARQLQuery> new_req_queue;

    Reply_Map rmap; // a map of replies for pending (fork-join) queries
    pthread_spinlock_t rmap_lock;

    vector<Message> pending_msgs;

    inline void sweep_msgs() {
        if (!pending_msgs.size()) return;

        cout << "[INFO]#" << tid << " " << pending_msgs.size() << " pending msgs on engine." << endl;
        for (vector<Message>::iterator it = pending_msgs.begin(); it != pending_msgs.end();)
            if (adaptor->send(it->sid, it->tid, it->bundle))
                it = pending_msgs.erase(it);
            else
                ++it;
    }

    bool send_request(Bundle &bundle, int dst_sid, int dst_tid) {
        if (adaptor->send(dst_sid, dst_tid, bundle))
            return true;

        // failed to send, then stash the msg to void deadlock
        pending_msgs.push_back(Message(dst_sid, dst_tid, bundle));
        return false;
    }


    void const_to_known(SPARQLQuery &req) { assert(false); } /// TODO

    void const_to_unknown(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        std::vector<sid_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;

        // the query plan is wrong
        assert(result.get_col_num() == 0);

        uint64_t sz = 0;
        edge_t *res = graph->get_edges_global(tid, start, d, pid, &sz);
        for (uint64_t k = 0; k < sz; k++)
            updated_result_table.push_back(res[k].val);

        result.result_table.swap(updated_result_table);
        result.add_var2col(end, 0);
        result.set_col_num(1);
        req.step++;
    }

    // all of these means const attribute
    void const_to_unknown_attr(SPARQLQuery & req ) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t aid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        std::vector<attr_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;

        assert(d == OUT); // attribute always uses OUT
        int type = SID_t;

        attr_t v;
        graph->get_vertex_attr_global(tid, start, d, aid, v);
        updated_result_table.push_back(v);
        type = boost::apply_visitor(get_type, v);

        result.attr_res_table.swap(updated_result_table);
        result.add_var2col(end, 0, type);
        result.set_attr_col_num(1);
        req.step++;
    }

    void known_to_unknown(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        std::vector<sid_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;

        // the query plan is wrong
        //assert(req.get_col_num() == req.var2col(end));

        updated_result_table.reserve(result.result_table.size());
        for (int i = 0; i < result.get_row_num(); i++) {
            sid_t prev_id = result.get_row_col(i, result.var2col(start));
            uint64_t sz = 0;
            edge_t *res = graph->get_edges_global(tid, prev_id, d, pid, &sz);
            for (uint64_t k = 0; k < sz; k++) {
                result.append_row_to(i, updated_result_table);
                updated_result_table.push_back(res[k].val);
            }
        }

        result.result_table.swap(updated_result_table);
        result.add_var2col(end, result.get_col_num());
        result.set_col_num(result.get_col_num() + 1);
        req.step++;
    }

    void known_to_unknown_attr(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        std::vector<attr_t> updated_attr_result_table;
        std::vector<sid_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;

        // attribute always uses OUT
        assert(d == OUT);
        int type = SID_t;

        updated_attr_result_table.reserve(result.attr_res_table.size());
        for (int i = 0; i < result.get_row_num(); i++) {
            sid_t prev_id = result.get_row_col(i, result.var2col(start));
            attr_t v;
            bool has_value = graph->get_vertex_attr_global(tid, prev_id, d, pid, v);
            if (has_value) {
                result.append_row_to(i, updated_result_table);
                result.append_attr_row_to(i, updated_attr_result_table);
                updated_attr_result_table.push_back(v);
                type = boost::apply_visitor(get_type, v);
            }
        }

        result.attr_res_table.swap(updated_attr_result_table);
        result.result_table.swap(updated_result_table);
        result.add_var2col(end, result.get_attr_col_num(), type);
        result.set_attr_col_num(result.get_attr_col_num() + 1);
        req.step++;
    }

    void known_to_known(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        vector<sid_t> updated_result_table;
        vector<attr_t> updated_attr_res_table;
        SPARQLQuery::Result &result = req.result;

        for (int i = 0; i < result.get_row_num(); i++) {
            sid_t prev_id = result.get_row_col(i, result.var2col(start));
            uint64_t sz = 0;
            edge_t *res = graph->get_edges_global(tid, prev_id, d, pid, &sz);
            sid_t end2 = result.get_row_col(i, result.var2col(end));
            for (uint64_t k = 0; k < sz; k++) {
                if (res[k].val == end2) {
                    result.append_row_to(i, updated_result_table);
                    if (global_enable_vattr)
                        result.append_attr_row_to(i, updated_attr_res_table);
                    break;
                }
            }
        }

        result.result_table.swap(updated_result_table);
        if (global_enable_vattr)
            result.attr_res_table.swap(updated_attr_res_table);
        req.step++;
    }

    void known_to_const(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        vector<sid_t> updated_result_table;
        vector<attr_t> updated_attr_res_table;
        SPARQLQuery::Result &result = req.result;

        for (int i = 0; i < result.get_row_num(); i++) {
            sid_t prev_id = result.get_row_col(i, result.var2col(start));
            uint64_t sz = 0;
            edge_t *res = graph->get_edges_global(tid, prev_id, d, pid, &sz);
            for (uint64_t k = 0; k < sz; k++) {
                if (res[k].val == end) {
                    result.append_row_to(i, updated_result_table);
                    if (global_enable_vattr)
                        result.append_attr_row_to(i, updated_attr_res_table);
                    break;
                }
            }
        }

        result.result_table.swap(updated_result_table);
        if (global_enable_vattr)
            result.attr_res_table.swap(updated_attr_res_table);
        req.step++;
    }

    void index_to_unknown(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t tpid = pattern.subject;
        ssid_t id01   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t var   = pattern.object;
        vector<sid_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;

        assert(id01 == PREDICATE_ID || id01 == TYPE_ID); // predicate or type index

        assert(result.get_col_num() == 0); // the query plan is wrong

        uint64_t sz = 0;
        edge_t *res = graph->get_index_edges_local(tid, tpid, d, &sz);
        int start = req.tid;
        if (start < 0) {
            start = - start - 1;    // request from the same server
            for (uint64_t k = start; k < sz; k += global_mt_threshold - 1)
                updated_result_table.push_back(res[k].val);
        } else {
            for (uint64_t k = start; k < sz; k += global_mt_threshold)
                updated_result_table.push_back(res[k].val);
        }

        result.result_table.swap(updated_result_table);
        result.set_col_num(1);
        result.add_var2col(var, 0);
        req.step++;
        req.local_var = -1;
    }

    // e.g., "<http://www.Department0.University0.edu> ?P ?X"
    void const_unknown_unknown(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        vector<sid_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;

        // the query plan is wrong
        assert(result.get_col_num() == 0);

        uint64_t npids = 0;
        edge_t *pids = graph->get_edges_global(tid, start, d, PREDICATE_ID, &npids);

        // use a local buffer to store "known" predicates
        edge_t *tpids = (edge_t *)malloc(npids * sizeof(edge_t));
        memcpy((char *)tpids, (char *)pids, npids * sizeof(edge_t));

        for (uint64_t p = 0; p < npids; p++) {
            uint64_t sz = 0;
            edge_t *res = graph->get_edges_global(tid, start, d, tpids[p].val, &sz);
            for (uint64_t k = 0; k < sz; k++) {
                updated_result_table.push_back(tpids[p].val);
                updated_result_table.push_back(res[k].val);
            }
        }

        free(tpids);

        result.result_table.swap(updated_result_table);
        result.set_col_num(2);
        result.add_var2col(pid, 0);
        result.add_var2col(end, 1);
        req.step++;
    }

    // e.g., "<http://www.University0.edu> ub:subOrganizationOf ?D"
    //       "?D ?P ?X"
    void known_unknown_unknown(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        vector<sid_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;

        for (int i = 0; i < result.get_row_num(); i++) {
            sid_t prev_id = result.get_row_col(i, result.var2col(start));
            uint64_t npids = 0;
            edge_t *pids = graph->get_edges_global(tid, prev_id, d, PREDICATE_ID, &npids);

            // use a local buffer to store "known" predicates
            edge_t *tpids = (edge_t *)malloc(npids * sizeof(edge_t));
            memcpy((char *)tpids, (char *)pids, npids * sizeof(edge_t));

            for (uint64_t p = 0; p < npids; p++) {
                uint64_t sz = 0;
                edge_t *res = graph->get_edges_global(tid, prev_id, d, tpids[p].val, &sz);
                for (uint64_t k = 0; k < sz; k++) {
                    result.append_row_to(i, updated_result_table);
                    updated_result_table.push_back(tpids[p].val);
                    updated_result_table.push_back(res[k].val);
                }
            }

            free(tpids);
        }

        result.result_table.swap(updated_result_table);
        result.set_col_num(result.get_col_num() + 2);
        result.add_var2col(pid, result.get_col_num());
        result.add_var2col(end, result.get_col_num() + 1);
        req.step++;
    }

    // FIXME: deadcode
    void known_unknown_const(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        ssid_t pid   = pattern.predicate;
        dir_t d      = pattern.direction;
        ssid_t end   = pattern.object;
        vector<sid_t> updated_result_table;
        SPARQLQuery::Result &result = req.result;

        for (int i = 0; i < result.get_row_num(); i++) {
            sid_t prev_id = result.get_row_col(i, result.var2col(start));
            uint64_t npids = 0;
            edge_t *pids = graph->get_edges_global(tid, prev_id, d, PREDICATE_ID, &npids);

            // use a local buffer to store "known" predicates
            edge_t *tpids = (edge_t *)malloc(npids * sizeof(edge_t));
            memcpy((char *)tpids, (char *)pids, npids * sizeof(edge_t));

            for (uint64_t p = 0; p < npids; p++) {
                uint64_t sz = 0;
                edge_t *res = graph->get_edges_global(tid, prev_id, d, tpids[p].val, &sz);
                for (uint64_t k = 0; k < sz; k++) {
                    if (res[k].val == end) {
                        result.append_row_to(i, updated_result_table);
                        updated_result_table.push_back(tpids[p].val);
                        break;
                    }
                }
            }

            free(tpids);
        }

        result.add_var2col(pid, result.get_col_num());
        result.set_col_num(result.get_col_num() + 1);
        result.result_table.swap(updated_result_table);
        req.step++;
    }

    vector<SPARQLQuery> generate_optional_query(SPARQLQuery &req) {
        int size = req.pattern_group.optional.size();
        vector<SPARQLQuery> optional_reqs(size);
        for (int i = 0; i < size; i++) {
            optional_reqs[i].pid = req.id;
            optional_reqs[i].pattern_group = req.pattern_group.optional[i];
            optional_reqs[i].step = 0;
            optional_reqs[i].result = req.result;
            optional_reqs[i].result.blind = false;
        }
        return optional_reqs;
    }

    vector<SPARQLQuery> generate_union_query(SPARQLQuery &req) {
        int size = req.pattern_group.unions.size();
        vector<SPARQLQuery> union_reqs(size);
        for (int i = 0; i < size; i++) {
            union_reqs[i].pid = req.id;
            union_reqs[i].pattern_group = req.pattern_group.unions[i];
            if (union_reqs[i].start_from_index()
                    && (global_mt_threshold * global_num_servers > 1))
                union_reqs[i].force_dispatch = true;

            union_reqs[i].step = 0;
            union_reqs[i].result = req.result;
            union_reqs[i].result.blind = false;
        }
        return union_reqs;
    }

    vector<SPARQLQuery> generate_sub_query(SPARQLQuery &req) {
        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;

        // generate sub requests for all servers
        vector<SPARQLQuery> sub_reqs(global_num_servers);
        for (int i = 0; i < global_num_servers; i++) {
            sub_reqs[i].pid = req.id;
            sub_reqs[i].pattern_group = req.pattern_group;
            sub_reqs[i].step = req.step;
            sub_reqs[i].corun_step = req.corun_step;
            sub_reqs[i].fetch_step = req.fetch_step;
            sub_reqs[i].local_var = start;
            sub_reqs[i].priority = req.priority + 1;

            sub_reqs[i].result.col_num = req.result.col_num;
            sub_reqs[i].result.blind = req.result.blind;
            sub_reqs[i].result.v2c_map  = req.result.v2c_map;
            sub_reqs[i].result.nvars  = req.result.nvars;
        }

        // group intermediate results to servers
        for (int i = 0; i < req.result.get_row_num(); i++) {
            int dst_sid = mymath::hash_mod(req.result.get_row_col(i, req.result.var2col(start)),
                                           global_num_servers);
            req.result.append_row_to(i, sub_reqs[dst_sid].result.result_table);
        }

        return sub_reqs;
    }

    // fork-join or in-place execution
    bool need_fork_join(SPARQLQuery &req) {
        // always need fork-join mode w/o RDMA
        if (!global_use_rdma) return true;

        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start = pattern.subject;
        return ((req.local_var != start)
                && (req.result.get_row_num() >= global_rdma_threshold));
    }

    void do_corun(SPARQLQuery &req) {
        SPARQLQuery::Result &req_result = req.result;
        int corun_step = req.corun_step;
        int fetch_step = req.fetch_step;

        // step.1 remove dup;
        uint64_t t0 = timer::get_usec();

        boost::unordered_set<sid_t> unique_set;
        ssid_t vid = req.get_pattern(corun_step).subject;
        assert(vid < 0);
        int col_idx = req_result.var2col(vid);
        for (int i = 0; i < req_result.get_row_num(); i++)
            unique_set.insert(req_result.get_row_col(i, col_idx));

        // step.2 generate cmd_chain for sub-reqs
        SPARQLQuery::PatternGroup subgroup;
        vector<int> pvars_map; // from new_id to col_idx of id

        boost::unordered_map<sid_t, sid_t> sub_pvars;

        auto lambda = [&](ssid_t id) -> ssid_t {
            if (id < 0) { // remap pattern variable
                if (sub_pvars.find(id) == sub_pvars.end()) {
                    sid_t new_id = - (sub_pvars.size() + 1); // starts from -1
                    sub_pvars[id] = new_id;
                    pvars_map.push_back(req_result.var2col(id));
                }
                return sub_pvars[id];
            } else {
                return id;
            }
        };

        for (int i = corun_step; i < fetch_step; i++) {
            SPARQLQuery::Pattern &pattern = req.get_pattern(i);
            ssid_t subject = lambda(pattern.subject);
            ssid_t predicate = lambda(pattern.predicate);
            dir_t direction = pattern.direction;
            ssid_t object = lambda(pattern.object);
            SPARQLQuery::Pattern newPattern(subject, predicate, direction, object);
            newPattern.pred_type = 0;
            subgroup.patterns.push_back(newPattern);
        }

        // step.3 make sub-req
        SPARQLQuery sub_req;
        SPARQLQuery::Result &sub_result = sub_req.result;

        // query
        sub_req.pattern_group = subgroup;
        sub_result.nvars = pvars_map.size();

        // result
        boost::unordered_set<sid_t>::iterator iter;
        for (iter = unique_set.begin(); iter != unique_set.end(); iter++)
            sub_result.result_table.push_back(*iter);
        sub_result.col_num = 1;

        //init var_map
        sub_result.add_var2col(sub_pvars[vid], 0);

        sub_result.blind = false; // must take back results
        uint64_t t1 = timer::get_usec(); // time to generate the sub-request

        // step.4 execute sub-req
        while (true) {
            execute_one_step(sub_req);
            if (sub_req.is_finished())
                break;
        }
        uint64_t t2 = timer::get_usec(); // time to run the sub-request

        uint64_t t3, t4;
        vector<sid_t> updated_result_table;

        if (sub_result.get_col_num() > 2) { // qsort
            mytuple::qsort_tuple(sub_result.get_col_num(), sub_result.result_table);

            t3 = timer::get_usec();
            vector<sid_t> tmp_vec;
            tmp_vec.resize(sub_result.get_col_num());
            for (int i = 0; i < req_result.get_row_num(); i++) {
                for (int c = 0; c < pvars_map.size(); c++)
                    tmp_vec[c] = req_result.get_row_col(i, pvars_map[c]);

                if (mytuple::binary_search_tuple(sub_result.get_col_num(),
                                                 sub_result.result_table, tmp_vec))
                    req_result.append_row_to(i, updated_result_table);
            }
            t4 = timer::get_usec();
        } else { // hash join
            boost::unordered_set<int64_pair> remote_set;
            for (int i = 0; i < sub_result.get_row_num(); i++)
                remote_set.insert(int64_pair(sub_result.get_row_col(i, 0),
                                             sub_result.get_row_col(i, 1)));

            t3 = timer::get_usec();
            vector<sid_t> tmp_vec;
            tmp_vec.resize(sub_result.get_col_num());
            for (int i = 0; i < req_result.get_row_num(); i++) {
                for (int c = 0; c < pvars_map.size(); c++)
                    tmp_vec[c] = req_result.get_row_col(i, pvars_map[c]);

                int64_pair target = int64_pair(tmp_vec[0], tmp_vec[1]);
                if (remote_set.find(target) != remote_set.end())
                    req_result.append_row_to(i, updated_result_table);
            }
            t4 = timer::get_usec();
        }

        // debug
        if (sid == 0 && tid == 0) {
            cout << "prepare " << (t1 - t0) << " us" << endl;
            cout << "execute sub-request " << (t2 - t1) << " us" << endl;
            cout << "sort " << (t3 - t2) << " us" << endl;
            cout << "lookup " << (t4 - t3) << " us" << endl;
        }

        req_result.result_table.swap(updated_result_table);
        req.step = fetch_step;
    }

    bool execute_one_step(SPARQLQuery &req) {
        if (req.is_finished())
            return false;

        if (req.step == 0 && req.start_from_index()) {
            index_to_unknown(req);
            return true;
        }

        SPARQLQuery::Pattern &pattern = req.get_current_pattern();
        ssid_t start     = pattern.subject;
        ssid_t predicate = pattern.predicate;
        dir_t direction  = pattern.direction;
        ssid_t end       = pattern.object;

        // triple pattern with unknown predicate/attribute
        if (predicate < 0) {
#ifdef VERSATILE
            switch (const_pair(req.result.variable_type(start),
                               req.result.variable_type(end))) {
            case const_pair(const_var, unknown_var):
                const_unknown_unknown(req);
                break;
            case const_pair(known_var, unknown_var):
                known_unknown_unknown(req);
                break;
            default:
                cout << "ERROR: unsupported triple pattern with unknown predicate "
                     << "(" << req.result.variable_type(start)
                     << "|" << req.result.variable_type(end)
                     << ")" << endl;
                assert(false);
            }
            return true;
#else
            cout << "ERROR: unsupported variable at predicate." << endl;
            cout << "Please add definition VERSATILE in CMakeLists.txt." << endl;
            assert(false);
#endif
        }

        // triple pattern with attribute
        if (global_enable_vattr && req.get_pattern(req.step).pred_type > 0) {
            switch (const_pair(req.result.variable_type(start),
                               req.result.variable_type(end))) {
            case const_pair(const_var, unknown_var):
                const_to_unknown_attr(req);
                break;
            case const_pair(known_var, unknown_var):
                known_to_unknown_attr(req);
                break;
            default:
                cout << "ERROR: unsupported triple pattern with attribute "
                     << "(" << req.result.variable_type(start)
                     << "|" << req.result.variable_type(end)
                     << ")" << endl;
                assert(false);
            }
            return true;
        }

        // triple pattern with known predicate
        switch (const_pair(req.result.variable_type(start),
                           req.result.variable_type(end))) {

        // start from const
        case const_pair(const_var, const_var):
            cout << "ERROR: unsupported triple pattern (from const to const)" << endl;
            assert(false);
        case const_pair(const_var, known_var):
            cout << "ERROR: unsupported triple pattern (from const to known)" << endl;
            assert(false);
        case const_pair(const_var, unknown_var):
            const_to_unknown(req);
            break;

        // start from known
        case const_pair(known_var, const_var):
            known_to_const(req);
            break;
        case const_pair(known_var, known_var):
            known_to_known(req);
            break;
        case const_pair(known_var, unknown_var):
            known_to_unknown(req);
            break;

        // start from unknown
        case const_pair(unknown_var, const_var):
        case const_pair(unknown_var, known_var):
        case const_pair(unknown_var, unknown_var):
            cout << "ERROR: unsupported triple pattern (from unknown)" << endl;
            assert(false);

        default:
            cout << "ERROR: unsupported triple pattern with known predicate "
                 << "(" << req.result.variable_type(start)
                 << "|" << req.result.variable_type(end)
                 << ")" << endl;
            assert(false);
        }

        return true;
    }

    // relational operator: < <= > >= == !=
    void relational_filter(SPARQLQuery::Filter &filter,
                           SPARQLQuery::Result &result,
                           vector<bool> &is_satisfy) {
        int col1 = (filter.arg1->type == SPARQLQuery::Filter::Type::Variable)
                   ? result.var2col(filter.arg1->valueArg) : -1;
        int col2 = (filter.arg2->type == SPARQLQuery::Filter::Type::Variable)
                   ? result.var2col(filter.arg2->valueArg) : -1;

        auto get_str = [&](SPARQLQuery::Filter & filter, int row, int col) -> string {
            int id = 0;
            switch (filter.type) {
            case SPARQLQuery::Filter::Type::Variable:
                id = result.get_row_col(row, col);
                return str_server->exist(id) ? str_server->id2str[id] : "";
            case SPARQLQuery::Filter::Type::Literal:
                return "\"" + filter.value + "\"";
            default:
                cout << "filter type not supported currently" << endl;
                assert(false);
            }
            return "";
        };

        switch (filter.type) {
        case SPARQLQuery::Filter::Type::Equal:
            for (int row = 0; row < result.get_row_num(); row ++)
                if (is_satisfy[row]
                        && (get_str(*filter.arg1, row, col1)
                            != get_str(*filter.arg2, row, col2)))
                    is_satisfy[row] = false;
            break;
        case SPARQLQuery::Filter::Type::NotEqual:
            for (int row = 0; row < result.get_row_num(); row ++)
                if (is_satisfy[row]
                        && (get_str(*filter.arg1, row, col1)
                            == get_str(*filter.arg2, row, col2)))
                    is_satisfy[row] = false;
            break;
        case SPARQLQuery::Filter::Type::Less:
            for (int row = 0; row < result.get_row_num(); row ++)
                if (is_satisfy[row]
                        && (get_str(*filter.arg1, row, col1)
                            >= get_str(*filter.arg2, row, col2)))
                    is_satisfy[row] = false;
            break;
        case SPARQLQuery::Filter::Type::LessOrEqual:
            for (int row = 0; row < result.get_row_num(); row ++)
                if (is_satisfy[row]
                        && (get_str(*filter.arg1, row, col1)
                            > get_str(*filter.arg2, row, col2)))
                    is_satisfy[row] = false;
            break;
        case SPARQLQuery::Filter::Type::Greater:
            for (int row = 0; row < result.get_row_num(); row ++)
                if (is_satisfy[row]
                        && (get_str(*filter.arg1, row, col1)
                            <= get_str(*filter.arg2, row, col2)))
                    is_satisfy[row] = false;
            break;
        case SPARQLQuery::Filter::Type::GreaterOrEqual:
            for (int row = 0; row < result.get_row_num(); row ++)
                if (is_satisfy[row]
                        && get_str(*filter.arg1, row, col1)
                        < get_str(*filter.arg2, row, col2))
                    is_satisfy[row] = false;
            break;
        }
    }

    void bound_filter(SPARQLQuery::Filter &filter,
                      SPARQLQuery::Result &result,
                      vector<bool> &is_satisfy) {
        int col = result.var2col(filter.arg1 -> valueArg);

        for (int row = 0; row < is_satisfy.size(); row ++) {
            if (!is_satisfy[row])
                continue;

            if (result.get_row_col(row, col) == BLANK_ID)
                is_satisfy[row] = false;
        }
    }

    // IRI and URI are the same in SPARQL
    void is_IRI_filter(SPARQLQuery::Filter &filter,
                       SPARQLQuery::Result &result,
                       vector<bool> &is_satisfy) {
        int col = result.var2col(filter.arg1->valueArg);

        string IRI_REF = R"(<([^<>\\"{}|^`\\])*>)";
        string prefixed_name = ".*:.*";
        string IRIref_str = "(" + IRI_REF + "|" + prefixed_name + ")";

        regex IRI_pattern(IRIref_str);
        for (int row = 0; row < is_satisfy.size(); row ++) {
            if (!is_satisfy[row])
                continue;

            int id = result.get_row_col(row, col);
            string str = str_server->exist(id) ? str_server->id2str[id] : "";
            if (!regex_match(str, IRI_pattern))
                is_satisfy[row] = false;
        }
    }

    void is_literal_filter(SPARQLQuery::Filter &filter,
                           SPARQLQuery::Result &result,
                           vector<bool> &is_satisfy) {
        int col = result.var2col(filter.arg1->valueArg);

        string langtag_pattern_str("@[a-zA-Z]+(-[a-zA-Z0-9]+)*");

        string literal1_str = R"('([^\x27\x5C\x0A\x0D]|\\[tbnrf\"'])*')";
        string literal2_str = R"("([^\x22\x5C\x0A\x0D]|\\[tbnrf\"'])*")";
        string literal_long1_str = R"('''(('|'')?([^'\\]|\\[tbnrf\"']))*''')";
        string literal_long2_str = R"("""(("|"")?([^"\\]|\\[tbnrf\"']))*""")";
        string literal = "(" + literal1_str + "|" + literal2_str + "|"
                         + literal_long1_str + "|" + literal_long2_str + ")";

        string IRI_REF = R"(<([^<>\\"{}|^`\\])*>)";
        string prefixed_name = ".*:.*";
        string IRIref_str = "(" + IRI_REF + "|" + prefixed_name + ")";

        regex RDFLiteral_pattern(literal + "(" + langtag_pattern_str + "|(\\^\\^" + IRIref_str +  "))?");

        for (int row = 0; row < is_satisfy.size(); row ++) {
            if (!is_satisfy[row])
                continue;

            int id = result.get_row_col(row, col);
            string str = str_server->exist(id) ? str_server->id2str[id] : "";
            if (!regex_match(str, RDFLiteral_pattern))
                is_satisfy[row] = false;
        }
    }

    // regex flag only support "i" option now
    void regex_filter(SPARQLQuery::Filter &filter,
                      SPARQLQuery::Result &result,
                      vector<bool> &is_satisfy) {
        regex pattern;
        if (filter.arg3 != nullptr && filter.arg3->value == "i")
            pattern = regex(filter.arg2->value, std::regex::icase);
        else
            pattern = regex(filter.arg2->value);

        int col = result.var2col(filter.arg1->valueArg);
        for (int row = 0; row < is_satisfy.size(); row ++) {
            if (!is_satisfy[row])
                continue;

            int id = result.get_row_col(row, col);
            string str = str_server->exist(id) ? str_server->id2str[id] : "";
            if (str.front() != '\"' || str.back() != '\"')
                cout << "the first parameter of function regex can only be string" << endl;
            else
                str = str.substr(1, str.length() - 2);

            if (!regex_match(str, pattern))
                is_satisfy[row] = false;
        }
    }

    void general_filter(SPARQLQuery::Filter &filter,
                        SPARQLQuery::Result &result,
                        vector<bool> &is_satisfy) {
        // conditional operator
        if (filter.type <= 1) {
            vector<bool> is_satisfy1(result.get_row_num(), true);
            vector<bool> is_satisfy2(result.get_row_num(), true);
            if (filter.type == SPARQLQuery::Filter::Type::And) {
                general_filter(*filter.arg1, result, is_satisfy);
                general_filter(*filter.arg2, result, is_satisfy);
            } else if (filter.type == SPARQLQuery::Filter::Type::Or) {
                general_filter(*filter.arg1, result, is_satisfy1);
                general_filter(*filter.arg2, result, is_satisfy2);
                for (int i = 0; i < is_satisfy.size(); i ++)
                    is_satisfy[i] = is_satisfy[i] && (is_satisfy1[i] || is_satisfy2[i]);
            }
        }
        // relational operator
        else if (filter.type <= 7)
            return relational_filter(filter, result, is_satisfy);
        else if (filter.type == SPARQLQuery::Filter::Type::Builtin_bound)
            return bound_filter(filter, result, is_satisfy);
        else if (filter.type == SPARQLQuery::Filter::Type::Builtin_isiri)
            return is_IRI_filter(filter, result, is_satisfy);
        else if (filter.type == SPARQLQuery::Filter::Type::Builtin_isliteral)
            return is_literal_filter(filter, result, is_satisfy);
        else if (filter.type == SPARQLQuery::Filter::Type::Builtin_regex)
            return regex_filter(filter, result, is_satisfy);

    }

    void filter(SPARQLQuery &r) {
        if (r.pattern_group.filters.size() == 0) return;

        // during filtering, flag of unsatified row will be set to false one by one
        vector<bool> is_satisfy(r.result.get_row_num(), true);

        for (int i = 0; i < r.pattern_group.filters.size(); i ++) {
            SPARQLQuery::Filter filter = r.pattern_group.filters[i];
            general_filter(filter, r.result, is_satisfy);
        }

        vector<sid_t> new_table;
        for (int row = 0; row < r.result.get_row_num(); row ++) {
            if (is_satisfy[row]) {
                r.result.append_row_to(row, new_table);
            }
        }
        r.result.result_table.swap(new_table);
        r.result.row_num = r.result.get_row_num();
    }

    class Compare {
    private:
        SPARQLQuery &query;
        String_Server *str_server;
    public:
        Compare(SPARQLQuery &query, String_Server *str_server)
            : query(query), str_server(str_server) { }

        bool operator()(const int* a, const int* b) {
            int cmp = 0;
            for (int i = 0; i < query.orders.size(); i ++) {
                int col = query.result.var2col(query.orders[i].id);
                string str_a = str_server->exist(a[col]) ? str_server->id2str[a[col]] : "";
                string str_b = str_server->exist(a[col]) ? str_server->id2str[b[col]] : "";
                cmp = str_a.compare(str_b);
                if (cmp != 0) {
                    cmp = query.orders[i].descending ? -cmp : cmp;
                    break;
                }
            }
            return cmp < 0;
        }
    };

    class ReduceCmp {
    private:
        int col_num;
    public:
        ReduceCmp(int col_num): col_num(col_num) { }

        bool operator()(const int* a, const int* b) {
            for (int i = 0; i < col_num; i ++) {
                if (a[i] == b[i])
                    continue;
                return a[i] < b[i];
            }
            return 0;
        }
    };

    void execute_optional(SPARQLQuery &r) {
        r.optional_dispatched = true;
        vector<SPARQLQuery> optional_reqs = generate_optional_query(r);
        rmap.put_parent_request(r, optional_reqs.size());
        for (int i = 0; i < optional_reqs.size(); i++) {
            if (need_fork_join(optional_reqs[i])) {
                optional_reqs[i].id = coder.get_and_inc_qid();
                vector<SPARQLQuery> sub_reqs = generate_sub_query(optional_reqs[i]);
                rmap.put_parent_request(optional_reqs[i], sub_reqs.size());
                for (int j = 0; j < sub_reqs.size(); j++) {
                    if (j != sid) {
                        Bundle bundle(sub_reqs[j]);
                        send_request(bundle, j, tid);
                    } else {
                        pthread_spin_lock(&recv_lock);
                        msg_fast_path.push_back(sub_reqs[j]);
                        pthread_spin_unlock(&recv_lock);
                    }
                }
            } else {
                int dst_sid = mymath::hash_mod(optional_reqs[i].pattern_group.patterns[0].subject, global_num_servers);
                if (dst_sid != sid) {
                    Bundle bundle(optional_reqs[i]);
                    send_request(bundle, i, tid);
                } else {
                    pthread_spin_lock(&recv_lock);
                    msg_fast_path.push_back(optional_reqs[i]);
                    pthread_spin_unlock(&recv_lock);
                }
            }
        }
    }

    void final_process(SPARQLQuery &r) {
        if (r.result.blind || r.result.result_table.size() == 0)
            return;

        // DISTINCT and ORDER BY
        if (r.distinct || r.orders.size() > 0) {
            // initialize table
            int **table;
            int size = r.result.get_row_num();
            int new_size = size;

            table = new int*[size];
            for (int i = 0; i < size; i ++)
                table[i] = new int[r.result.col_num];

            for (int i = 0; i < size; i ++)
                for (int j = 0; j < r.result.col_num; j ++)
                    table[i][j] = r.result.get_row_col(i, j);

            // DISTINCT
            if (r.distinct) {
                // sort and then compare
                sort(table, table + size, ReduceCmp(r.result.col_num));
                int p = 0, q = 1;
                auto equal = [&r](int *a, int *b) -> bool{
                    for (int i = 0; i < r.result.required_vars.size(); i ++) {
                        int col = r.result.var2col(r.result.required_vars[i]);
                        if (a[col] != b[col]) return false;
                    }
                    return true;
                };
                auto swap = [](int *&a, int *&b) {
                    int *temp = a;
                    a = b;
                    b = temp;
                };
                while (q < size) {
                    while (equal(table[p], table[q])) {
                        q++;
                        if (q >= size) goto out;
                    }
                    p ++;
                    swap(table[p], table[q]);
                    q ++;
                }
out:
                new_size = p + 1;
            }
            // ORDER BY
            if (r.orders.size() > 0) {
                sort(table, table + new_size, Compare(r, str_server));
            }
            //write back data and delete **table
            for (int i = 0; i < new_size; i ++)
                for (int j = 0; j < r.result.col_num; j ++)
                    r.result.result_table[r.result.col_num * i + j] = table[i][j];

            if (new_size < size)
                r.result.result_table.erase(r.result.result_table.begin() + new_size * r.result.col_num,
                                            r.result.result_table.begin() + size * r.result.col_num);

            for (int i = 0; i < size; i ++)
                delete[] table[i];
            delete[] table;
        }

        // OFFSET
        if (r.offset > 0)
            r.result.result_table.erase(r.result.result_table.begin(),
                                        min(r.result.result_table.begin()
                                            + r.offset * r.result.col_num,
                                            r.result.result_table.end()));

        // LIMIT
        if (r.limit >= 0)
            r.result.result_table.erase(min(r.result.result_table.begin()
                                            + r.limit * r.result.col_num,
                                            r.result.result_table.end()),
                                        r.result.result_table.end() );

        // remove unrequested variables
        int new_col_num = r.result.required_vars.size();
        int new_row_num = r.result.get_row_num();
        vector<sid_t> new_result_table(new_row_num * new_col_num);
        for (int i = 0; i < new_row_num; i ++) {
            for (int j = 0; j < new_col_num; j ++) {
                int col = r.result.var2col(r.result.required_vars[j]);
                new_result_table[i * new_col_num + j] = r.result.get_row_col(i, col);
            }
        }

        r.result.result_table.swap(new_result_table);
        r.result.col_num = new_col_num;
    }

    void execute_sparql_request(SPARQLQuery &r) {
        SPARQLQuery::Result &result = r.result;
        r.id = coder.get_and_inc_qid();
        // if r starts from index and is from proxy, dispatch it to every engine except itself
        if (r.force_dispatch
                || (r.step == 0
                    && coder.tid_of(r.pid) < global_num_proxies
                    && r.start_from_index()
                    && global_mt_threshold * global_num_servers > 1)) {
            int sub_reqs_size = global_num_servers * global_mt_threshold - 1;

            rmap.put_parent_request(r, sub_reqs_size);
            SPARQLQuery sub_query = r;
            sub_query.force_dispatch = false;
            for (int i = 0; i < global_num_servers; i++) {
                for (int j = 0; j < global_mt_threshold; j++) {
                    sub_query.id = -1;
                    sub_query.pid = r.id;
                    sub_query.tid = j; // specified engine

                    if (i == sid && j + global_num_proxies == tid) {
                        //dispatching engine thread shall do nothing
                        continue;
                    } else if (i == sid && j + global_num_proxies > tid) {
                        sub_query.tid -- ;  // [0, infinity)
                        sub_query.tid = - sub_query.tid - 1; // means send to the same server
                    } else if (i == sid) {
                        sub_query.tid = - sub_query.tid - 1; // means send to the same server
                    }

                    Bundle bundle(sub_query);
                    send_request(bundle, i, global_num_proxies + j);
                }
            }
            return;
        }

        while (true) {
            uint64_t t1 = timer::get_usec();
            execute_one_step(r);
            t1 = timer::get_usec() - t1;

            // co-run optimization
            if (!r.is_finished() && (r.step == r.corun_step))
                do_corun(r);

            if (r.is_finished()) {
                // union, when union or optional occurs, filters will be delayed till they are processed.
                if (r.is_union()) {
                    vector<SPARQLQuery> union_reqs = generate_union_query(r);
                    rmap.put_parent_request(r, union_reqs.size());
                    for (int i = 0; i < union_reqs.size(); i++) {
                        int dst_sid = mymath::hash_mod(union_reqs[i].pattern_group.patterns[0].subject,
                                                       global_num_servers);
                        if (dst_sid != sid) {
                            Bundle bundle(union_reqs[i]);
                            send_request(bundle, i, tid);
                        } else {
                            pthread_spin_lock(&recv_lock);
                            msg_fast_path.push_back(union_reqs[i]);
                            pthread_spin_unlock(&recv_lock);
                        }
                    }
                    return;
                }

                if (!r.is_union() && !r.is_optional()) {
                    // result should be filtered at the end of every distributed query
                    // because FILTER could be nested in every PatternGroup
                    filter(r);
                }

                // if all data has been merged and next will be sent back to proxy
                if (coder.tid_of(r.pid) < global_num_proxies) {
                    if (r.is_optional() && !r.optional_dispatched) {
                        execute_optional(r);
                        return;
                    }
                    final_process(r);
                }

                result.row_num = result.get_row_num();
                r.clear_data();
                Bundle bundle(r);
                send_request(bundle, coder.sid_of(r.pid), coder.tid_of(r.pid));
                return;
            }

            if (need_fork_join(r)) {
                vector<SPARQLQuery> sub_reqs = generate_sub_query(r);
                rmap.put_parent_request(r, sub_reqs.size());
                for (int i = 0; i < sub_reqs.size(); i++) {
                    if (i != sid) {
                        Bundle bundle(sub_reqs[i]);
                        send_request(bundle, i, tid);
                    } else {
                        pthread_spin_lock(&recv_lock);
                        msg_fast_path.push_back(sub_reqs[i]);
                        pthread_spin_unlock(&recv_lock);
                    }
                }
                return;
            }
        }
        return;
    }

    void execute_sparql_reply(SPARQLQuery &r, Engine *engine) {
        pthread_spin_lock(&engine->rmap_lock);
        engine->rmap.put_reply(r);

        if (engine->rmap.is_ready(r.pid)) {
            SPARQLQuery reply = engine->rmap.get_merged_reply(r.pid);
            pthread_spin_unlock(&engine->rmap_lock);

            // optional will be processed after union , and filter follows.
            if (reply.is_optional() || (!reply.is_optional() && reply.is_union()))
                filter(reply);

            // if all data has been merged and next will be sent back to proxy
            if (coder.tid_of(reply.pid) < global_num_proxies) {
                if (reply.is_optional() && !reply.optional_dispatched) {
                    execute_optional(reply);
                    return;
                }
                final_process(reply);
            }
            Bundle bundle(reply);
            send_request(bundle, coder.sid_of(reply.pid), coder.tid_of(reply.pid));
        } else {
            pthread_spin_unlock(&engine->rmap_lock);
        }
    }

    void execute_sparql_query(SPARQLQuery &r, Engine *engine) {
        if (r.is_request())
            execute_sparql_request(r);
        else
            execute_sparql_reply(r, engine);
    }

#if DYNAMIC_GSTORE
    void execute_load_data(RDFLoad &r) {
        r.load_ret = graph->dynamic_load_data(r.load_dname, r.check_dup);
        Bundle bundle(r);
        send_request(bundle, coder.sid_of(r.pid), coder.tid_of(r.pid));
    }
#endif

    void execute_gstore_check(GStoreCheck& r) {
        r.check_ret = graph->gstore_check(r.index_check, r.normal_check);
        Bundle bundle(r);
        send_request(bundle, coder.sid_of(r.pid), coder.tid_of(r.pid));
    }

    void execute(Bundle &bundle, Engine *engine) {
        if (bundle.type == SPARQL_QUERY) {
            SPARQLQuery r = bundle.get_sparql_query();
            execute_sparql_query(r, engine);
        }
#if DYNAMIC_GSTORE
        else if (bundle.type == DYNAMIC_LOAD) {
            RDFLoad r = bundle.get_rdf_load();
            execute_load_data(r);
        }
#endif
        else if (bundle.type == GSTORE_CHECK) {
            GStoreCheck r = bundle.get_gstore_check();
            execute_gstore_check(r);
        }
    }

public:
    const static uint64_t TIMEOUT_THRESHOLD = 10000; // 10 msec

    int sid;    // server id
    int tid;    // thread id

    String_Server *str_server;
    DGraph *graph;
    Adaptor *adaptor;

    Coder coder;

    uint64_t last_time; // busy or not (work-oblige)

    Engine(int sid, int tid, String_Server *str_server, DGraph *graph, Adaptor *adaptor)
        : sid(sid), tid(tid), str_server(str_server), graph(graph), adaptor(adaptor),
          coder(sid, tid), last_time(0) {
        pthread_spin_init(&recv_lock, 0);
        pthread_spin_init(&rmap_lock, 0);
    }

    void run() {
        // NOTE: the 'tid' of engine is not start from 0,
        // which can not be used by engines[] directly
        int own_id = tid - global_num_proxies;
        // TODO: replace pair to ring
        int nbr_id = (global_num_engines - 1) - own_id;

        uint64_t last_recv_time = timer::get_usec();
        uint64_t snooze_time = MIN_SNOOZE_TIME;

        auto set_recv_flag = [&last_recv_time, &snooze_time](bool &has_msg) {
            has_msg = true; // keep calm (no snooze)
            snooze_time = MIN_SNOOZE_TIME;
            last_recv_time = timer::get_usec();
        };
        while (true) {
            Bundle bundle;
            bool has_msg = false;

            // fast path
            last_time = timer::get_usec();

            SPARQLQuery request;
            pthread_spin_lock(&recv_lock);
            if (msg_fast_path.size() > 0) {
                set_recv_flag(has_msg);
                request = msg_fast_path[0];
                msg_fast_path.erase(msg_fast_path.begin());

            }
            pthread_spin_unlock(&recv_lock);

            if (has_msg) {
                execute_sparql_query(request, engines[own_id]);
                continue; // fast-path priority
            }

            // check and send pending messages
            sweep_msgs();

            // normal path
            last_time = timer::get_usec();

            // own queue
            while (adaptor->tryrecv(bundle)) {
                if (bundle.type == SPARQL_QUERY) {
                    SPARQLQuery req = bundle.get_sparql_query();
                    if (req.priority != 0) {
                        set_recv_flag(has_msg);
                        execute_sparql_query(req, engines[own_id]);
                        break;
                    } else {
                        new_req_queue.push_back(req);
                    }
                } else {
                    set_recv_flag(has_msg);
                    execute(bundle, engines[own_id]);
                    break;
                }
            }
            if (!has_msg && new_req_queue.size() > 0) {
                set_recv_flag(has_msg);
                SPARQLQuery req = new_req_queue[0];
                new_req_queue.erase(new_req_queue.begin());
                execute_sparql_query(req, engines[own_id]);
            }

            // work-oblige is enabled
            // FIXME: the job should be stealed from new_req_queue
            if (global_enable_workstealing)  {
                // if neighboring worker is not self-sufficient, try to get job
                last_time = timer::get_usec();
                if ((last_time >= engines[nbr_id]->last_time + TIMEOUT_THRESHOLD) // neighbor is busy
                        && engines[nbr_id]->adaptor->tryrecv(bundle)) {
                    set_recv_flag(has_msg);
                    execute(bundle, engines[nbr_id]);
                }
            }

            if (has_msg) continue; // keep calm (no snooze)

            // busy polling a little while (BUSY_POLLING_THRESHOLD) before snooze
            if (snooze_time > MIN_SNOOZE_TIME // snooze again (no need polling again)
                    || (timer::get_usec() - last_recv_time) > BUSY_POLLING_THRESHOLD) {
                thread_delay(snooze_time); // release CPU (snooze)

                // double snooze time till MAX_SNOOZE_TIME
                snooze_time *= snooze_time < MAX_SNOOZE_TIME ? 2 : 1;
            }
        }
    }
};
