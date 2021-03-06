//  Copyright (c) 2007-2015 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_PARALLEL_UTIL_FOREACH_PARTITIONER_OCT_03_2014_0112PM)
#define HPX_PARALLEL_UTIL_FOREACH_PARTITIONER_OCT_03_2014_0112PM

#include <hpx/hpx_fwd.hpp>
#include <hpx/async.hpp>
#include <hpx/exception_list.hpp>
#include <hpx/lcos/wait_all.hpp>
#include <hpx/lcos/local/dataflow.hpp>
#include <hpx/util/bind.hpp>
#include <hpx/util/decay.hpp>
#include <hpx/util/deferred_call.hpp>
#include <hpx/util/invoke_fused.hpp>
#include <hpx/util/tuple.hpp>

#include <hpx/parallel/executors/executor_traits.hpp>
#include <hpx/parallel/execution_policy.hpp>
#include <hpx/parallel/util/detail/chunk_size.hpp>
#include <hpx/parallel/util/detail/handle_local_exceptions.hpp>
#include <hpx/parallel/traits/extract_partitioner.hpp>

#include <algorithm>

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace parallel { namespace util
{
    namespace detail
    {
        ///////////////////////////////////////////////////////////////////////
        // The static partitioner simply spawns one chunk of iterations for
        // each available core.
        template <typename ExPolicy, typename Result = void>
        struct foreach_n_static_partitioner
        {
            template <typename FwdIter, typename F1>
            static FwdIter call(ExPolicy policy, FwdIter first,
                std::size_t count, F1 && f1, std::size_t chunk_size)
            {
                typedef typename ExPolicy::executor_type executor_type;
                typedef typename hpx::parallel::executor_traits<executor_type>
                    executor_traits;
                typedef typename hpx::util::tuple<FwdIter, std::size_t> tuple;

                FwdIter last = first;
                std::advance(last, count);

                std::vector<hpx::future<Result> > inititems, workitems;
                std::list<boost::exception_ptr> errors;
                std::vector<tuple> shape;

                try {
                    // estimates a chunk size based on number of cores used
                    shape = get_static_shape(policy, inititems, f1,
                        first, count, chunk_size);

                    workitems.reserve(shape.size());

                    using hpx::util::bind;
                    using hpx::util::functional::invoke_fused;
                    using hpx::util::placeholders::_1;
                    workitems = executor_traits::async_execute(
                        policy.executor(),
                        bind(invoke_fused(), std::forward<F1>(f1), _1),
                        shape);
                }
                catch (...) {
                    detail::handle_local_exceptions<ExPolicy>::call(
                        boost::current_exception(), errors);
                }

                // wait for all tasks to finish
                hpx::wait_all(inititems);
                hpx::wait_all(workitems);

                // handle exceptions
                detail::handle_local_exceptions<ExPolicy>::call(
                    inititems, errors);
                detail::handle_local_exceptions<ExPolicy>::call(
                    workitems, errors);

                return last;
            }
        };

        ///////////////////////////////////////////////////////////////////////
        template <typename Result>
        struct foreach_n_static_partitioner<parallel_task_execution_policy, Result>
        {
            template <typename ExPolicy, typename FwdIter, typename F1>
            static hpx::future<FwdIter> call(ExPolicy policy,
                FwdIter first, std::size_t count, F1 && f1,
                std::size_t chunk_size)
            {
                typedef typename ExPolicy::executor_type executor_type;
                typedef typename hpx::parallel::executor_traits<executor_type>
                    executor_traits;
                typedef typename hpx::util::tuple<FwdIter, std::size_t> tuple;

                FwdIter last = first;
                std::advance(last, count);

                std::vector<hpx::future<Result> > inititems, workitems;
                std::list<boost::exception_ptr> errors;
                std::vector<tuple> shape;

                try {
                    // estimates a chunk size based on number of cores used
                    shape = get_static_shape(policy, inititems, f1,
                        first, count, chunk_size);

                    workitems.reserve(shape.size());

                    using hpx::util::bind;
                    using hpx::util::functional::invoke_fused;
                    using hpx::util::placeholders::_1;
                    workitems = executor_traits::async_execute(
                        policy.executor(),
                        bind(invoke_fused(), std::forward<F1>(f1), _1),
                        shape);
                }
                catch (std::bad_alloc const&) {
                    return hpx::make_exceptional_future<FwdIter>(
                        boost::current_exception());
                }
                catch (...) {
                    errors.push_back(boost::current_exception());
                }

                // wait for all tasks to finish
                return hpx::lcos::local::dataflow(
                    [last, errors](std::vector<hpx::future<Result> > && r1,
                            std::vector<hpx::future<Result> > && r2)
                        mutable -> FwdIter
                    {
                        detail::handle_local_exceptions<ExPolicy>::call(r1, errors);
                        detail::handle_local_exceptions<ExPolicy>::call(r2, errors);
                        return last;
                    },
                    std::move(inititems), std::move(workitems));
            }
        };

        template <typename Executor, typename Result>
        struct foreach_n_static_partitioner<
                parallel_task_execution_policy_shim<Executor>, Result>
          : foreach_n_static_partitioner<parallel_task_execution_policy, Result>
        {};

        ///////////////////////////////////////////////////////////////////////
        // ExPolicy: execution policy
        // Result:   intermediate result type of first step (default: void)
        // PartTag:  select appropriate partitioner
        template <typename ExPolicy, typename Result, typename PartTag>
        struct foreach_n_partitioner;

        ///////////////////////////////////////////////////////////////////////
        template <typename ExPolicy, typename Result>
        struct foreach_n_partitioner<ExPolicy, Result,
            parallel::traits::static_partitioner_tag>
        {
            template <typename FwdIter, typename F1>
            static FwdIter call(ExPolicy policy, FwdIter first,
                std::size_t count, F1 && f1, std::size_t chunk_size = 0)
            {
                return foreach_n_static_partitioner<ExPolicy, Result>::call(
                    policy, first, count, std::forward<F1>(f1), chunk_size);
            }
        };

        template <typename Result>
        struct foreach_n_partitioner<parallel_task_execution_policy, Result,
                parallel::traits::static_partitioner_tag>
        {
            template <typename ExPolicy, typename FwdIter, typename F1>
            static hpx::future<FwdIter> call(ExPolicy policy,
                FwdIter first, std::size_t count, F1 && f1,
                std::size_t chunk_size = 0)
            {
                return foreach_n_static_partitioner<ExPolicy, Result>::call(
                    policy, first, count, std::forward<F1>(f1), chunk_size);
            }
        };

        template <typename Executor, typename Result>
        struct foreach_n_partitioner<
                parallel_task_execution_policy_shim<Executor>, Result,
                parallel::traits::static_partitioner_tag>
          : foreach_n_partitioner<parallel_task_execution_policy, Result,
                parallel::traits::static_partitioner_tag>
        {};

        template <typename Executor, typename Result>
        struct foreach_n_partitioner<
                parallel_task_execution_policy_shim<Executor>, Result,
                parallel::traits::auto_partitioner_tag>
          : foreach_n_partitioner<parallel_task_execution_policy, Result,
                parallel::traits::auto_partitioner_tag>
        {};

        template <typename Executor, typename Result>
        struct foreach_n_partitioner<
                parallel_task_execution_policy_shim<Executor>, Result,
                parallel::traits::default_partitioner_tag>
          : foreach_n_partitioner<parallel_task_execution_policy, Result,
                parallel::traits::static_partitioner_tag>
        {};

        ///////////////////////////////////////////////////////////////////////
        template <typename ExPolicy, typename Result>
        struct foreach_n_partitioner<ExPolicy, Result,
                parallel::traits::default_partitioner_tag>
          : foreach_n_partitioner<ExPolicy, Result,
                parallel::traits::static_partitioner_tag>
        {};
    }

    ///////////////////////////////////////////////////////////////////////////
    template <typename ExPolicy, typename Result = void,
        typename PartTag = typename parallel::traits::extract_partitioner<
            typename hpx::util::decay<ExPolicy>::type
        >::type>
    struct foreach_n_partitioner
      : detail::foreach_n_partitioner<
            typename hpx::util::decay<ExPolicy>::type, Result, PartTag>
    {};
}}}

#endif
