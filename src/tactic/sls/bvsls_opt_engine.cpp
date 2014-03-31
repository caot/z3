/*++
Copyright (c) 2014 Microsoft Corporation

Module Name:

    bvsls_opt_engine.cpp

Abstract:

    Optimization extensions to bvsls

Author:

    Christoph (cwinter) 2014-03-28

Notes:

--*/
#include "nnf.h"
#include "bvsls_opt_engine.h"

bvsls_opt_engine::bvsls_opt_engine(ast_manager & m, params_ref const & p) :
    sls_engine(m, p),
    m_hard_tracker(sls_engine::m_tracker),
    m_obj_tracker(m, m_bv_util, m_mpz_manager, m_powers),
    m_obj_evaluator(m, m_bv_util, m_obj_tracker, m_mpz_manager, m_powers)
{
    m_best_model = alloc(model, m);
}

bvsls_opt_engine::~bvsls_opt_engine()
{    
}

bvsls_opt_engine::optimization_result bvsls_opt_engine::optimize(
    expr_ref const & objective, 
    model_ref initial_model, 
    bool _maximize)
{
    SASSERT(m_bv_util.is_bv(objective));
    TRACE("sls_opt", tout << "objective: " << (_maximize?"maximize":"minimize") << " " <<
                            mk_ismt2_pp(objective, m()) << std::endl;);
    m_hard_tracker.initialize(m_assertions);
    m_restart_limit = _RESTART_LIMIT_;    

    if (initial_model.get() != 0) {
        TRACE("sls_opt", tout << "Initial model provided: " << std::endl;
                        for (unsigned i = 0; i < initial_model->get_num_constants(); i++) {
                            func_decl * fd = initial_model->get_constant(i);
                            expr * val = initial_model->get_const_interp(fd);
                            tout << fd->get_name() << " := " << mk_ismt2_pp(val, m()) << std::endl;
                        });
        m_hard_tracker.set_model(initial_model);
    }

    optimization_result res(m_manager);

    res.is_sat = m_hard_tracker.is_sat() ? l_true : l_undef;

    if (!res.is_sat) {
        do {
            checkpoint();

            IF_VERBOSE(1, verbose_stream() << "Satisfying... restarts left:" << (m_max_restarts - m_stats.m_restarts) << std::endl;);
            res.is_sat = search();

            if (res.is_sat == l_undef)
                m_hard_tracker.randomize(m_assertions);
        } while (m_stats.m_stopwatch.get_current_seconds() < _TIMELIMIT_ &&
                 res.is_sat != l_true && m_stats.m_restarts++ < m_max_restarts);
    }    

    if (res.is_sat)
        res.optimum = _maximize ? maximize(objective) : minimize(objective);

    TRACE("sls_opt", tout << "sat: " << res.is_sat << "; optimum: " << mk_ismt2_pp(res.optimum, m()) << std::endl;);

    return res;
}

expr_ref bvsls_opt_engine::maximize(expr_ref const & objective)
{
    SASSERT(m_hard_tracker.is_sat());

    expr * obj_e = objective.get();
    unsigned obj_bv_sz = m_bv_util.get_bv_size(obj_e);        
    ptr_vector<expr> objs;
    objs.push_back(obj_e);
    m_obj_tracker.initialize(objs);    
    m_obj_evaluator.update_all();
    m_obj_tracker.set_model(m_hard_tracker.get_model());

    TRACE("sls_opt", tout << "Initial opt model:" << std::endl; m_obj_tracker.show_model(tout););
    IF_VERBOSE(1, verbose_stream() << "Maximizing... restarts left:" << (m_max_restarts - m_stats.m_restarts) << std::endl;);    

    mpz score, old_score, max_score, new_value;
    unsigned new_const = (unsigned)-1, new_bit = 0;
    ptr_vector<func_decl> consts = m_obj_tracker.get_constants();
    move_type move;
    m_mpz_manager.set(score, m_obj_tracker.get_value(obj_e));
    m_mpz_manager.set(max_score, m_powers(obj_bv_sz)); m_mpz_manager.dec(max_score);

    save_model(score);    

    while (check_restart((unsigned)m_stats.m_stopwatch.get_current_seconds()) &&
           m_stats.m_stopwatch.get_current_seconds() < _TIMELIMIT_ &&
           m_mpz_manager.lt(score, max_score)) {
        checkpoint();
        m_stats.m_moves++;
        m_mpz_manager.set(old_score, score);
        new_const = (unsigned)-1;
        
        mpz score(0);
        m_mpz_manager.set(score,
                          find_best_move(consts, score, new_const, new_value, new_bit, move, max_score, obj_e));        

        if (new_const == static_cast<unsigned>(-1)) {
            m_mpz_manager.set(score, old_score);
            if (m_mpz_manager.gt(score, m_best_model_score))
                save_model(score);            
            if (!randomize_wrt_hard()) {
                // Can't improve and can't randomize; can't do anything other than bail out.
                TRACE("sls_opt", tout << "Got stuck; bailing out." << std::endl;);
                goto bailout;
            }
            m_mpz_manager.set(score, top_score());            
        }
        else {
            TRACE("sls_opt", tout << "New best: " << m_mpz_manager.to_string(score) << std::endl;);
            func_decl * fd = consts[new_const];
            incremental_score(fd, new_value);
            m_mpz_manager.set(score, top_score());
        }
    }

bailout:
    m_mpz_manager.del(new_value);
    
    expr_ref res(m_manager);
    res = m_bv_util.mk_numeral(m_best_model_score, obj_bv_sz);
    return res;
}

expr_ref bvsls_opt_engine::minimize(expr_ref const & objective)
{
    expr_ref n_obj(m_manager);
    n_obj = m_bv_util.mk_bv_neg(objective);
    return maximize(n_obj);
}

void bvsls_opt_engine::save_model(mpz const & score) {
    model_ref mdl = m_hard_tracker.get_model();
    model_ref obj_mdl = m_obj_tracker.get_model();

    for (unsigned i = 0; i < obj_mdl->get_num_constants(); i++) {
        func_decl * fd = obj_mdl->get_constant(i);
        expr * val = obj_mdl->get_const_interp(fd);
        if (mdl->has_interpretation(fd)) {
            if (mdl->get_const_interp(fd) != val)
                TRACE("sls_opt", tout << "model disagreement on " << fd->get_name() << ": " <<
                mk_ismt2_pp(val, m()) << " != " << mk_ismt2_pp(mdl->get_const_interp(fd), m()) << std::endl;);
            SASSERT(mdl->get_const_interp(fd) == val);
        }
        else
            mdl->register_decl(fd, val);
    }

    m_best_model = mdl;
    m_mpz_manager.set(m_best_model_score, score);
}

// checks whether the score outcome of a given move is better than the previous score
bool bvsls_opt_engine::what_if(
    func_decl * fd,
    const unsigned & fd_inx,
    const mpz & temp,
    mpz & best_score,
    unsigned & best_const,
    mpz & best_value) 
{
#if _EARLY_PRUNE_
    double r = incremental_score_prune(fd, temp);
#else
    double r = incremental_score(fd, temp);
#endif
    
    if (r >= 1.0 && m_tracker.is_sat()) {
        m_obj_evaluator.update(fd, temp);
        mpz cur_best(0);
        m_mpz_manager.set(cur_best, top_score());

        TRACE("sls_whatif", tout << "WHAT IF " << fd->get_name() << " WERE " << m_mpz_manager.to_string(temp) <<
              " --> " << r << "; score=" << m_mpz_manager.to_string(cur_best) << std::endl;);

        if (m_mpz_manager.gt(cur_best, best_score)) {
            m_mpz_manager.set(best_score, cur_best);
            best_const = fd_inx;
            m_mpz_manager.set(best_value, temp);
            return true;
        }
    }

    return false;
}

mpz bvsls_opt_engine::find_best_move(
    ptr_vector<func_decl> & to_evaluate,
    mpz score,
    unsigned & best_const,
    mpz & best_value,
    unsigned & new_bit,
    move_type & move,
    mpz const & max_score,
    expr * objective)
{
    mpz old_value, temp;
#if _USE_MUL3_ || _USE_UNARY_MINUS_
    mpz temp2;
#endif
    unsigned bv_sz;
    mpz new_score = score;

    for (unsigned i = 0; i < to_evaluate.size() && m_mpz_manager.lt(new_score, max_score); i++) {
        func_decl * fd = to_evaluate[i];
        sort * srt = fd->get_range();
        bv_sz = (m_manager.is_bool(srt)) ? 1 : m_bv_util.get_bv_size(srt);
        m_mpz_manager.set(old_value, m_tracker.get_value(fd));

        // first try to flip every bit
        for (unsigned j = 0; j < bv_sz && m_mpz_manager.lt(new_score, max_score); j++) {
            // What would happen if we flipped bit #i ?                
            mk_flip(srt, old_value, j, temp);

            if (what_if(fd, i, temp, new_score, best_const, best_value)) {
                new_bit = j;
                move = MV_FLIP;
            }
        }

        if (m_bv_util.is_bv_sort(srt) && bv_sz > 1) {
#if _USE_ADDSUB_
            if (!m_mpz_manager.is_even(old_value)) {
                // for odd values, try +1
                mk_inc(bv_sz, old_value, temp);
                if (what_if(fd, i, temp, new_score, best_const, best_value))
                    move = MV_INC;
            }
            else {
                // for even values, try -1
                mk_dec(bv_sz, old_value, temp);
                if (what_if(fd, i, temp, new_score, best_const, best_value))
                    move = MV_DEC;
            }
#endif
            // try inverting
            mk_inv(bv_sz, old_value, temp);
            if (what_if(fd, i, temp, new_score, best_const, best_value))
                move = MV_INV;

#if _USE_UNARY_MINUS_
            mk_inc(bv_sz, temp, temp2);
            if (what_if(fd, i, temp2, new_score, best_const, best_value))
                move = MV_UMIN;
#endif

#if _USE_MUL2DIV2_
            // try multiplication by 2
            mk_mul2(bv_sz, old_value, temp);
            if (what_if(fd, i, temp, new_score, best_const, best_value))
                move = MV_MUL2;

#if _USE_MUL3_
            // try multiplication by 3
            mk_add(bv_sz, old_value, temp, temp2);
            if (what_if(fd, i, temp2, new_score, best_const, best_value))
                move = MV_MUL3;
#endif

            // try division by 2
            mk_div2(bv_sz, old_value, temp);
            if (what_if(fd, i, temp, new_score, best_const, best_value))
                move = MV_DIV2;
#endif
        }

        // reset to what it was before
        double check = incremental_score(fd, old_value);
    }

    m_mpz_manager.del(old_value);
    m_mpz_manager.del(temp);
#if _USE_MUL3_
    m_mpz_manager.del(temp2);
#endif

    return new_score;    
}

bool bvsls_opt_engine::randomize_wrt_hard() {
    ptr_vector<func_decl> consts = m_obj_tracker.get_constants();
    unsigned csz = consts.size();
    unsigned retry_count = csz;

    while (retry_count-- > 0)  
    {
        
        unsigned ri = (m_tracker.get_random_uint((csz < 16) ? 4 : (csz < 256) ? 8 : (csz < 4096) ? 12 : (csz < 65536) ? 16 : 32)) % csz;
        func_decl * random_fd = consts[ri]; // Random constant
        
        mpz random_val; // random value.
        m_mpz_manager.set(random_val, m_tracker.get_random(random_fd->get_range()));        
        
        mpz old_value;
        m_mpz_manager.set(old_value, m_tracker.get_value(random_fd));
        
        if (!m_mpz_manager.eq(random_val, old_value)) {
            m_evaluator.update(random_fd, random_val);

            if (m_hard_tracker.is_sat()) {
                TRACE("sls_opt", tout << "Randomizing " << random_fd->get_name() << " to " <<
                                         m_mpz_manager.to_string(random_val) << std::endl;);
                m_obj_evaluator.update(random_fd, random_val);
                return true;
            }
            else
                m_evaluator.update(random_fd, old_value);
        }
    }

    return false;
}