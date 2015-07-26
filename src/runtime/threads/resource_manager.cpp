//  Copyright (c) 2007-2013 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include <hpx/hpx_fwd.hpp>
#include <hpx/runtime/threads/thread_executor.hpp>
#include <hpx/runtime/threads/resource_manager.hpp>
#include <hpx/lcos/local/once.hpp>
#include <hpx/util/reinitializable_static.hpp>

#include <boost/thread/locks.hpp>

namespace hpx { namespace threads
    {
        ///////////////////////////////////////////////////////////////////////////
        resource_manager& resource_manager::get()
        {
            typedef util::reinitializable_static<resource_manager, tag> static_type;

            static_type instance;
            return instance.get();
        }

        ///////////////////////////////////////////////////////////////////////////
        resource_manager::resource_manager()
            : next_cookie_(0),
            punits_(get_os_thread_count()),
            topology_(get_topology())
        {}

        // Request an initial resource allocation
        std::size_t resource_manager::initial_allocation(
                detail::manage_executor* proxy, error_code& ec)
        {
            if (0 == proxy) {
                HPX_THROWS_IF(ec, bad_parameter,
                        "resource_manager::init_allocation",
                        "manage_executor pointer is a nullptr");
                return std::size_t(-1);
            }

            // ask executor for its policies
            error_code ec1(lightweight);
            std::size_t min_punits = proxy->get_policy_element(detail::min_concurrency, ec1);
            if (ec1) min_punits = 1;
            std::size_t max_punits = proxy->get_policy_element(detail::max_concurrency, ec1);
            if (ec1) max_punits = get_os_thread_count();

            // lock the resource manager from this point on
            boost::lock_guard<mutex_type> l(mtx_);

            // allocate initial resources for the given executor
            std::vector<std::pair<std::size_t, std::size_t> > cores =
                allocate_virt_cores(proxy, min_punits, max_punits, ec);
            if (ec) return std::size_t(-1);

            // attach the given proxy to this resource manager
            std::size_t cookie = ++next_cookie_;
            proxies_.insert(proxies_map_type::value_type(
                        cookie, proxy_data(proxy, std::move(cores))));

            if (&ec != &throws)
                ec = make_success_code();
            return cookie;
        }

        // Find 'desired' amount of processing units which have the given use count
        // (use count is the number of schedulers associated with a given processing
        // unit).
        //
        // the resource manager is locked while executing this function
        std::size_t resource_manager::reserve_processing_units(
                std::size_t use_count, std::size_t desired,
                std::vector<BOOST_SCOPED_ENUM(punit_status)>& available_punits)
        {
            std::size_t available = 0;
            for (std::size_t i = 0; i != punits_.size(); ++i)
            {
                if (use_count == punits_[i].use_count_)
                {
                    available_punits[i] = punit_status::reserved;
                    if (++available == desired)
                        break;
                }
            }
            return available;
        }

        std::size_t resource_manager::reserve_at_higher_use_count(
                std::size_t desired,
                std::vector<BOOST_SCOPED_ENUM(punit_status)>& available_punits)
        {
            std::size_t use_count = 1;
            std::size_t available = 0;

            while (available < desired)
            {
                available += reserve_processing_units(
                        use_count++, desired - available, available_punits);
            }

            return available;
        }


        // Instructs a scheduler proxy to free up a fixed number of resources
        // This is only a temporary release of resources.
        // The use count on the global core is decremented and the scheduler
        // proxy remembers the core as temporarily released
        // release_cores_to_min - scheduler should release all cores above its minimum
        // release_borrowed_cores - scheduler should release all its borrowed cores

        bool resource_manager::release_scheduler_resources(std::map<std::size_t, static_allocation_data>::iterator it, 
                std::size_t number_to_free, std::vector<BOOST_SCOPED_ENUM(punit_status)>& available_punits)
        {
            static_allocation_data st;
            std::size_t borrowed_cores;
            std::size_t owned_cores;

            st = (*it).second;

            proxies_map_type::iterator iter;
            iter = proxies_.find((*it).first);
            proxy_data& p = (*iter).second;

            if (number_to_free == release_borrowed_cores)
            {
                number_to_free = borrowed_cores = st.num_borrowed_cores;
            }

            else if (number_to_free == release_cores_to_min)
            {
                number_to_free = st.num_owned_cores - st.min_proxy_cores;
                borrowed_cores = 0;
            }

            else
            {
                borrowed_cores = 0;
            }

            owned_cores = number_to_free - borrowed_cores;

            if (number_to_free > 0)
            {
                for (coreids_type coreids : p.core_ids_) {
                    if (punits_[coreids.first].use_count_ > 0 || owned_cores > 0)
                    {
                        // The proxy remembers this processor as gone ..
                        // TODO

                        --punits_[coreids.first].use_count_;
                        
                        if (punits_[coreids.first].use_count_ > 0)
                        {
                            --owned_cores;
                        }


                        if (--number_to_free == 0)
                        {
                            return true;
                        }
                    }
                }
            }

            // The scheduler proxy does not have any cores available to free.
            return false;

        }


        // Instructs existing schedulers to release cores. Then tries to reserve
        // available cores for the new scheduler

        std::size_t resource_manager::release_cores_on_existing_scedulers(std::size_t number_to_free,
                std::vector<BOOST_SCOPED_ENUM(punit_status)>& available_punits)
        {
            // Ask previously allocated schedulers to release surplus cores, until either the request is satisfied, or we're out of schedulers.
            bool released_cores = false;

            std::map<std::size_t, static_allocation_data>::iterator it;

            for (it = proxies_static_allocation_data.begin();it!=proxies_static_allocation_data.end();it++)
            {
                if (release_scheduler_resources(it, number_to_free, available_punits))
                {
                    released_cores = true;
                }

            }

            std::size_t available;

            if (released_cores)
            {
                available = reserve_processing_units(0, number_to_free, available_punits);
            }
            else
            {
                available = 0;
            }

            return available;
        }

        // Tries to redistribute cores allocated to all schedulers proportional 
        // to each schedulers maximum punits 
        // and reserve any freed cores for the new scheduler.
        std::size_t resource_manager::redistribute_cores_among_all(std::size_t reserved, std::size_t min_punits, std::size_t max_punits,
                std::vector<BOOST_SCOPED_ENUM(punit_status)>& available_punits)
        {

            std::size_t available = 0;

            // Try to proportionally allocate cores to all schedulers w/o oversubscription. The proportions used will be
            // max_punits for each scheduler, except that no existing scheduler will be forced to increase the current allocation.
            if (proxies_static_allocation_data.size() > 1)
            {
                std::size_t total_minimum = min_punits;
                std::size_t total_allocated = reserved; // sum of cores that have been previously reserved and cores that were reserved during this allocation attempt.
                std::size_t num_schedulers = 1; // includes the current scheduler

                // total_allocated isnumber of cores allocated to new scheduler so far plus 
                // the number of 'owned' cores allocated to all existing schedulers.

                std::map<std::size_t, static_allocation_data>::iterator it;
                for (it == proxies_static_allocation_data.begin();it!=proxies_static_allocation_data.end();it++)
                {

                    static_allocation_data st = (*it).second;

                    if (st.num_owned_cores > st.min_proxy_cores)
                    {
                        ++num_schedulers;
                        total_minimum += st.min_proxy_cores;
                        total_allocated += st.num_owned_cores;
                    }
                }

                if (num_schedulers > 1 && total_minimum <= total_allocated)
                {
                    // We have found schedulers with cores greater than min. Moreover, the sum of all cores already allocated to
                    // existing schedulers can at least satisfy all mins (including the min requirement of the current scheduler).
                    std::size_t cookie = next_cookie_ + 1;

                    double total_desired = 0.0;
                    double scaling = 0.0;

                    std::map<std::size_t, static_allocation_data> scaled_static_allocation_data;	

                    static_allocation_data st;
                    st.min_proxy_cores = min_punits;
                    st.max_proxy_cores = max_punits;	
                    st.adjusted_desired = max_punits;

                    total_desired += st.adjusted_desired;

                    scaled_static_allocation_data.insert(std::map<std::size_t, static_allocation_data>::value_type(cookie , st)); 

                    for (it = proxies_static_allocation_data.begin();it!=proxies_static_allocation_data.end();it++)
                    {
                        st = (*it).second;
                        if (st.num_owned_cores > st.min_proxy_cores)
                        {
                            st.adjusted_desired = st.max_proxy_cores;
                            scaled_static_allocation_data.insert(std::map<std::size_t, static_allocation_data>::value_type((*it).first , st));
                            total_desired += st.adjusted_desired;
                        }
                    }

                    while (true)
                    {
                        scaling = total_allocated/total_desired;

                        for (it = scaled_static_allocation_data.begin();it!=scaled_static_allocation_data.end();it++)
                        {
                            static_allocation_data st = (*it).second;
                            st.scaled_allocation = st.adjusted_desired * scaling ;
                        }

                        roundup_scaled_allocations(scaled_static_allocation_data, total_allocated);

                        bool re_calculate = false;
                        std::map<std::size_t, static_allocation_data>::iterator iter;          
                        iter = scaled_static_allocation_data.end();
                        iter--;

                        for (it = scaled_static_allocation_data.begin();it!=iter;it++)
                        {
                            static_allocation_data st = (*it).second;

                            if (st.allocation > st.num_owned_cores)
                            {
                                double modifier = st.num_owned_cores/st.allocation;

                                // Reduce adjusted_desired by multiplying it with 'modifier', to try to bias allocation to the original size or less.
                                total_desired -= st.adjusted_desired * (1.0 - modifier);
                                st.adjusted_desired = modifier * st.adjusted_desired;

                                re_calculate = true;
                            }
                        }

                        if (re_calculate)
                        {
                            continue;
                        }

                        for (it = scaled_static_allocation_data.begin();it!=scaled_static_allocation_data.end();it++)
                        {
                            // Keep recursing until all allocations are no greater than desired (including the current scheduler).
                            static_allocation_data st = (*it).second;

                            if (st.allocation > st.min_proxy_cores)
                            {
                                double modifier = st.min_proxy_cores/st.allocation;

                                // Reduce adjustedDesired by multiplying with it 'modifier', to try to bias allocation to desired or less.
                                total_desired -= st.adjusted_desired * (1.0 - modifier);
                                st.adjusted_desired = modifier*st.adjusted_desired;
                                re_calculate = true;
                            }
                        }

                        if (re_calculate)
                        {
                            continue;
                        }

                        for (it = scaled_static_allocation_data.begin();it!=scaled_static_allocation_data.end();it++)
                        {
                            // Keep recursing until all allocations are at least minimum (including the current scheduler).
                            static_allocation_data st = (*it).second;

                            if (st.min_proxy_cores > st.allocation)
                            {
                                double new_desired = st.min_proxy_cores/scaling;

                                // Bias desired to get allocation closer to min.
                                total_desired += new_desired - st.adjusted_desired;
                                st.adjusted_desired = new_desired;

                                re_calculate = true;
                            }
                        }

                        if (re_calculate)
                        {
                            continue;
                        }
                        break;
                    } // end of while(true)

                    it = scaled_static_allocation_data.end();
                    it--;
                    st = (*it).second;

                    if (st.allocation > total_allocated)
                    {
                        std::map<std::size_t, static_allocation_data>::iterator iter;          
                        iter = scaled_static_allocation_data.end();
                        iter--;

                        for (it = scaled_static_allocation_data.begin();it!=iter;it++)
                        {

                            static_allocation_data st = (*it).second;

                            std::size_t reduce_by = st.num_owned_cores - st.allocation;
                            if (reduce_by > 0)
                            {
                                release_scheduler_resources(it, reduce_by, available_punits);
                            }
                        }

                        // Reserve out of the cores we just freed.
                        available = reserve_processing_units(0,st.allocation - reserved,available_punits);
                    }

                    scaled_static_allocation_data.clear();
                }
            }
            return available;
        }

        /// <summary>
        ///     Denote the doubles in the input array AllocationData[*].m_scaledAllocation by: r[1],..., r[n].
        ///     Split r[j] into b[j] and fract[j] where b[j] is the integral floor of r[j] and fract[j] is the fraction truncated.
        ///     Sort the set { r[j] | j = 1,...,n } from largest fract[j] to smallest.
        ///     For each j = 0, 1, 2,...  if fract[j] > 0, then set b[j] += 1 and pay for the cost of 1-fract[j] by rounding
        ///     fract[j0] -> 0 from the end (j0 = n-1, n-2,...) -- stop before j > j0. b[j] is stored in AllocationData[*].m_allocation.
        ///     totalAllocated is the sum of all AllocationData[*].m_scaledAllocation upon entry, which after the function call is over will
        ///     necessarily be equal to the sum of all AllocationData[*].m_allocation.
        /// </summary>
        void resource_manager::roundup_scaled_allocations(std::map<std::size_t, static_allocation_data> &scaled_static_allocation_data , std::size_t total_allocated)
        {

        }


        void resource_manager::preprocess_static_allocation()
        {
            proxies_map_type::iterator it;
            proxies_static_allocation_data.clear();

            for (it == proxies_.begin();it!=proxies_.end();it++)
            { 
                proxy_data& p = (*it).second;
                boost::shared_ptr<detail::manage_executor> proxy = p.proxy_;
                static_allocation_data st;
                st.proxy_ = proxy;

                // ask executor for its policies
                error_code ec1(lightweight);
                st.min_proxy_cores = proxy->get_policy_element(detail::min_concurrency, ec1);
                if (ec1) st.min_proxy_cores = 1;
                st.max_proxy_cores = proxy->get_policy_element(detail::max_concurrency, ec1);
                if (ec1) st.max_proxy_cores = get_os_thread_count();

                st.num_borrowed_cores = 0;
                st.num_owned_cores = 0;


                for (coreids_type coreids : p.core_ids_)
                {
                    if (punits_[coreids.first].use_count_ > 1)
                        st.num_borrowed_cores++;
                    if (punits_[coreids.first].use_count_ == 1)
                        st.num_owned_cores++;
                }

                proxies_static_allocation_data.insert(std::map<std::size_t, 
                        static_allocation_data>::value_type((*it).first , st));
            }

        }

        // the resource manager is locked while executing this function
        std::vector<std::pair<std::size_t, std::size_t> >
            resource_manager::allocate_virt_cores(
                    detail::manage_executor* proxy, std::size_t min_punits,
                    std::size_t max_punits, error_code& ec)
            {
                std::vector<coreids_type> core_ids;
               
                // array of available processing units
                std::vector<BOOST_SCOPED_ENUM(punit_status)> available_punits(
                        get_os_thread_count(), punit_status::unassigned);

                // find all available processing units with zero use count
                std::size_t reserved = reserve_processing_units(0, max_punits,
                        available_punits);
                if (reserved < max_punits)
                {
                    // insufficient available cores found, try to share 
                    // processing units

                    preprocess_static_allocation();

                    reserved+=release_cores_on_existing_scedulers(release_borrowed_cores, available_punits);

                    if(reserved < max_punits)
                    {
                        reserved += redistribute_cores_among_all(reserved, min_punits, max_punits,available_punits);

                        if (reserved < min_punits)
                        {
                            reserved += release_cores_on_existing_scedulers(release_cores_to_min, available_punits);
                            if (reserved < min_punits)
                            {
                                reserved += reserve_at_higher_use_count(min_punits - reserved , available_punits);
                            }
                        }
                    }
                }

                // processing units found, inform scheduler
                std::size_t punit = 0;
                for (std::size_t i = 0; i != available_punits.size(); ++i)
                {
                    if (available_punits[i] == punit_status::reserved) //-V104
                    {
                        proxy->add_processing_unit(punit, i, ec);
                        if (ec) break;

                        core_ids.push_back(std::make_pair(i, punit));
                        ++punit;

                        // update use count for reserved processing units
                        ++punits_[i].use_count_;
						
                    }

                }
                HPX_ASSERT(punit <= max_punits);

                if (ec) {
                    // on error, remove the already assigned virtual cores
                    for (std::size_t j = 0; j != punit; ++j)
                    {
                        proxy->remove_processing_unit(j, ec);
                        --punits_[j].use_count_;
                    }
                    
                    return std::vector<coreids_type>();

                }

                if (&ec != &throws)
                    ec = make_success_code();
                return core_ids;
            }


        // Stop the executor identified with the given cookie
        void resource_manager::stop_executor(std::size_t cookie, error_code& ec)
        {
            boost::lock_guard<mutex_type> l(mtx_);
            proxies_map_type::iterator it = proxies_.find(cookie);
            if (it == proxies_.end()) {
                HPX_THROWS_IF(ec, bad_parameter, "resource_manager::detach",
                        "the given cookie is not known to the resource manager");
                return;
            }

            // inform executor to give up virtual cores
            proxy_data& p = (*it).second;
            for (coreids_type coreids : p.core_ids_)
            {
                p.proxy_->remove_processing_unit(coreids.second, ec);
            }
        }

        // Detach the executor identified with the given cookie
        void resource_manager::detach(std::size_t cookie, error_code& ec)
        {
            boost::lock_guard<mutex_type> l(mtx_);
            proxies_map_type::iterator it = proxies_.find(cookie);
            if (it == proxies_.end()) {
                HPX_THROWS_IF(ec, bad_parameter, "resource_manager::detach",
                        "the given cookie is not known to the resource manager");
                return;
            }

            // adjust resource usage count
            proxy_data& p = (*it).second;
            for (coreids_type coreids : p.core_ids_)
            {
                HPX_ASSERT(punits_[coreids.first].use_count_ != 0);
                --punits_[coreids.first].use_count_;
            }

            proxies_.erase(cookie);
        }
    }}
