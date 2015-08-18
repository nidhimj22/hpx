//  Copyright (c) 2007-2013 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_RUNTIME_THREADS_RESOURCE_MANAGER_JAN_16_2013_0830AM)
#define HPX_RUNTIME_THREADS_RESOURCE_MANAGER_JAN_16_2013_0830AM

#include <hpx/hpx_fwd.hpp>
#include <hpx/util/move.hpp>
#include <hpx/runtime/threads/topology.hpp>
#include <hpx/runtime/threads/thread_executor.hpp>
#include <hpx/lcos/local/spinlock.hpp>
#include <boost/detail/scoped_enum_emulation.hpp>

#include <boost/atomic.hpp>
#include <boost/shared_ptr.hpp>

#include <vector>

namespace hpx { namespace  threads
    {
        ///////////////////////////////////////////////////////////////////////////
        /// Status of a given processing unit
        BOOST_SCOPED_ENUM_START(punit_status)
        {
            // Initial state for all cores.
            unknown = 0,

            // A core that is not assigned to a scheduler proxy, either because the scheduler proxy did
            // not ask for it (it already has what it desires), or because it is assigned to some other scheduler.
            // This state is only used for core in a scheduler's local view of resources.
            unassigned = 1,

            // The core is available and may be reserved for or allocated to a scheduler.
            available = 2,

            // The core is reserved for a scheduler by the Resource Manager. This state is only used for core
            // in a scheduler's local view of resources.
            reserved = 3,

            // The core is allocated to a scheduler. Cores transition from Reserved to Allocated once the
            // scheduler proxy has allocated execution resources or virtual processors for its scheduler.
            allocated = 4,

            // When a scheduler releases a core for a different scheduler, it sets its core state to Stolen.
            // This enables it to track the cores it has relinquished. If the core is not allocated to the
            // receiving scheduler, it will revert back to Allocated for the scheduler proxy it came from.
            stolen = 5,

            // A core is considered idle during dynamic core migration if the scheduler(s) that core is assigned
            // to, have all vprocs de-activated. This state is only used for cores in the global map.
            idle = 6
        };
        BOOST_SCOPED_ENUM_END

        ///////////////////////////////////////////////////////////////////////////
        /// Status of a dynamic worker thread
        BOOST_SCOPED_ENUM_START(dynamicworker_state)
        {
            standby = 0,
            load_balance = 1,
            exit_thread = 2
        };
        BOOST_SCOPED_ENUM_END

        typedef BOOST_SCOPED_ENUM(dynamicworker_state) dynamicworker_state;

        /// In short, there are two main responsibilities of the Resource Manager:
        ///
        /// * Initial Allocation: Allocating resources to executors when executors
        ///   are created.
        /// * Dynamic Migration: Constantly monitoring utilization of resources
        ///   by executors, and dynamically migrating resources between them
        ///   (not implemented yet).
        ///
        class resource_manager
        {
            typedef lcos::local::spinlock mutex_type;
            struct tag {};

            // mapping of physical core to virtual core
            typedef std::pair<std::size_t, std::size_t>  coreids_type;

            public:
            resource_manager();

            // Request an initial resource allocation
            std::size_t initial_allocation(detail::manage_executor* proxy,
                    error_code& ec = throws);

            // Stop the executor identified by the given cookie
            void stop_executor(std::size_t cookie, error_code& ec = throws);

            // Detach the executor identified by the given cookie
            void detach(std::size_t cookie, error_code& ec = throws);

            // Return the singleton resource manager instance
            static resource_manager& get();

            protected:
            std::vector<coreids_type> allocate_virt_cores(
                    detail::manage_executor* proxy, std::size_t min_punits,
                    std::size_t max_punits, error_code& ec);

            std::size_t reserve_processing_units(
                    std::size_t use_count, std::size_t desired,
                    std::vector<BOOST_SCOPED_ENUM(punit_status)>& available_punits);

            std::size_t reserve_at_higher_use_count(
                    std::size_t desired,
                    std::vector<BOOST_SCOPED_ENUM(punit_status)>& available_punits);

            private:
            mutable mutex_type mtx_;
            boost::atomic<std::size_t> next_cookie_;

            ///////////////////////////////////////////////////////////////////////
            // Store information about the physical processing units available to
            // this resource manager.
            struct punit_data
            {
                punit_data() : use_count_(0) {}

                std::size_t use_count_;   // number of schedulers using this core
            };

            typedef std::vector<punit_data> punit_array_type;
            punit_array_type punits_;

            threads::topology const& topology_;

            ///////////////////////////////////////////////////////////////////////
            // Store information about the virtual processing unit allocation for
            // each of the scheduler proxies attached to this resource manager.
            struct proxy_data
            {
                public:
                    proxy_data(detail::manage_executor* proxy,
                            std::vector<coreids_type> && core_ids)
                        : proxy_(proxy), core_ids_(std::move(core_ids))
                    {}

                    proxy_data(proxy_data const& rhs)
                        : proxy_(rhs.proxy_),
                        core_ids_(rhs.core_ids_)
                {}

                    proxy_data(proxy_data && rhs)
                        : proxy_(std::move(rhs.proxy_)),
                        core_ids_(std::move(rhs.core_ids_))
                {}

                    proxy_data& operator=(proxy_data const& rhs)
                    {
                        if (this != &rhs) {
                            proxy_ = rhs.proxy_;
                            core_ids_ = rhs.core_ids_;
                        }
                        return *this;
                    }

                    proxy_data& operator=(proxy_data && rhs)
                    {
                        if (this != &rhs) {
                            proxy_ = std::move(rhs.proxy_);
                            core_ids_ = std::move(rhs.core_ids_);
                        }
                        return *this;
                    }

                    boost::shared_ptr<detail::manage_executor> proxy_;  // hold on to proxy
                    std::vector<coreids_type> core_ids_;                // map physical to logical puinit ids
            };

            typedef std::map<std::size_t, proxy_data> proxies_map_type;
            proxies_map_type proxies_;


            //     Used to store information during static and dynamic allocation.

            struct allocation_data
            {
                // The scheduler proxy this allocation data is for.
                boost::shared_ptr<detail::manage_executor> proxy_;  // hold on to proxy

                // Additional allocation to give to a scheduler after proportional allocation decisions are made.
                std::size_t allocation;

                // Used to hold a scaled allocation value during proportional allocation.
                double scaled_allocation;

                std::size_t num_borrowed_cores;
                std::size_t num_owned_cores;
                std::size_t min_proxy_cores;
                std::size_t max_proxy_cores;
            };



            struct static_allocation_data : public allocation_data
            {
                // A field used during static allocation to decide on an allocation proportional to each scheduler's desired value.
                double adjusted_desired;
                // Keeps track of stolen cores during static allocation.
                std::size_t num_cores_stolen;
            };

            struct dynamic_allocation_data : public allocation_data
            {

                // Fully loaded is set to true when a scheduler is using all the cores that are allocated to it (no cores are idle)
                // AND it has less than its desired number of cores.
                bool fully_loaded;

                // Number suggested as an appropriate allocation for the scheduler proxy, by the hill climbing instance.
                std::size_t suggested_allocation;

                union
                {
                    // Struct used for a receiving proxy.
                    struct
                    {
                        // As we go through dynamic allocation, the starting node index moves along the array of sorted nodes,
                        // in a scheduling proxy that is receiving cores.
                        std::size_t starting_node;
                    };
                    // Struct used for a giving proxy.
                    struct
                    {
                        // Maximum number of borrowed idle cores this scheduler can give up.
                        std::size_t borrowed_idle_cores_to_migrate;

                        // Maximum number of borrowed in-use cores this scheduler can give up.
                        std::size_t borrowed_inuse_cores_to_migrate;

                        // Maximum number of owned cores this scheduler can give up.
                        std::size_t owned_cores_to_migrate;
                    };
                };
            };




            std::map<std::size_t, static_allocation_data> proxies_static_allocation_data;
            std::map<std::size_t, dynamic_allocation_data> proxies_dynamic_allocation_data;   


            void preprocess_static_allocation();
            void preprocess_dynamic_allocation();


            const size_t release_borrowed_cores = (std::size_t)-1;
            const size_t release_cores_to_min = (std::size_t)-2;

            bool release_scheduler_resources(std::map<std::size_t, static_allocation_data>::iterator it, 
                    std::size_t number_to_free, std::vector<BOOST_SCOPED_ENUM(punit_status)>& available_punits);

            std::size_t release_cores_on_existing_scedulers(std::size_t number_to_free,
                    std::vector<BOOST_SCOPED_ENUM(punit_status)>& available_punits);

            std::size_t redistribute_cores_among_all(std::size_t reserved, std::size_t min_punits, std::size_t max_punits,
                    std::vector<BOOST_SCOPED_ENUM(punit_status)>& available_punits);

            void roundup_scaled_allocations(std::map<std::size_t, static_allocation_data> &scaled_static_allocation_data , std::size_t total_allocated);

            volatile dynamicworker_state dynamicrmworker_state(standby);

            /// <summary>
            ///     Starts up the dynamic RM worker thread if it is on standby, or creates a thread if one is not already created.
            ///     The worker thread wakes up at fixed intervals and load balances resources among schedulers, until it it put on standby.
            /// </summary>
            void create_dynamicrm_worker();

            /// <summary>
            ///     Routine that performs dynamic resource management among existing schedulers at fixed time intervals.
            /// </summary>
            bool dynamic_resource_manager();

            void DistributeCoresToSurvivingScheduler();

        };
    }}


#endif
