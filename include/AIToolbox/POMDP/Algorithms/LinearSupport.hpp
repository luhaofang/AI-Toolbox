#ifndef AI_TOOLBOX_POMDP_LINEAR_SUPPORT_HEADER_FILE
#define AI_TOOLBOX_POMDP_LINEAR_SUPPORT_HEADER_FILE

#include <boost/heap/fibonacci_heap.hpp>

#include <AIToolbox/POMDP/Types.hpp>
#include <AIToolbox/POMDP/TypeTraits.hpp>
#include <AIToolbox/POMDP/Utils.hpp>
#include <AIToolbox/POMDP/Algorithms/Utils/Projecter.hpp>

#include <unordered_set>
#include <boost/iterator/transform_iterator.hpp>
#include <boost/functional/hash.hpp>

#include <AIToolbox/Utils/Polytope.hpp>

#include <AIToolbox/Impl/Logging.hpp>

namespace AIToolbox::POMDP {
    /**
     * @brief This class represents the LinearSupport algorithm.
     *
     * This method is similar in spirit to Witness. The idea is that we look at
     * certain belief points, and we try to find the best alphavectors in those
     * points. Rather than looking for them though, the idea here is that we
     * *know* where they are, if there are any at all.
     *
     * As the ValueFunction is piecewise linear and convex, if there's any
     * other hyperplane that we can add to improve it, the improvements are
     * going to be maximal at one of the vertices of the original surface.
     *
     * The idea thus is the following: first we compute the set of alphavectors
     * for the corners, so we can be sure about them. Then we find all vertices
     * that those alphavectors create, and we compute the error between the
     * true ValueFunction and their current values.
     *
     * If the error is greater than a certain amount, we allow their supporting
     * alphavector to join the ValueFunction, and we increase the size of the
     * vertex set by adding all new vertices that are created by adding the new
     * surface (and removing the ones that are made useless by it).
     *
     * We repeat until we have checked all available vertices, and at that
     * point we are done.
     *
     * While this can be a very inefficient algorithm, the fact that vertices
     * are checked in an orderly fashion, from highest error to lowest, allows
     * if one needs it to convert this algorithm into an anytime algorithm.
     * Even if there is limited time to compute the solution, the algorithm is
     * guaranteed to work in the areas with high error first, allowing one to
     * compute good approximations even without a lot of resources.
     */
    class LinearSupport {
        public:
            /**
             * @brief Basic constructor.
             *
             * This constructor sets the default horizon used to solve a POMDP::Model.
             *
             * The epsilon parameter must be >= 0.0, otherwise the
             * constructor will throw an std::runtime_error. The epsilon
             * parameter sets the convergence criterion. An epsilon of 0.0
             * forces LinearSupport to perform a number of iterations equal to
             * the horizon specified. Otherwise, LinearSupport will stop as soon
             * as the difference between two iterations is less than the
             * epsilon specified.
             *
             * @param h The horizon chosen.
             * @param epsilon The epsilon factor to stop the value iteration loop.
             */
            LinearSupport(unsigned horizon, double epsilon);

            /**
             * @brief This function sets the epsilon parameter.
             *
             * The epsilon parameter must be >= 0.0, otherwise the
             * constructor will throw an std::runtime_error. The epsilon
             * parameter sets the convergence criterion. An epsilon of 0.0
             * forces LinearSupport to perform a number of iterations equal to
             * the horizon specified. Otherwise, LinearSupport will stop as soon
             * as the difference between two iterations is less than the
             * epsilon specified.
             *
             * @param e The new epsilon parameter.
             */
            void setEpsilon(double e);

            /**
             * @brief This function allows setting the horizon parameter.
             *
             * @param h The new horizon parameter.
             */
            void setHorizon(unsigned h);

            /**
             * @brief This function will return the currently set epsilon parameter.
             *
             * @return The currently set epsilon parameter.
             */
            double getEpsilon() const;

            /**
             * @brief This function returns the currently set horizon parameter.
             *
             * @return The current horizon.
             */
            unsigned getHorizon() const;

            /**
             * @brief This function solves a POMDP::Model completely.
             *
             * This function is pretty expensive (as are possibly all POMDP
             * solvers). It evaluates all vertices in the ValueFunction surface
             * in order to determine whether it is complete, otherwise it
             * improves it incrementally.
             *
             * @tparam M The type of POMDP model that needs to be solved.
             *
             * @param model The POMDP model that needs to be solved.
             *
             * @return A tuple containing the maximum variation for the
             *         ValueFunction and the computed ValueFunction.
             */
            template <typename M, typename = std::enable_if_t<is_model_v<M>>>
            std::tuple<double, ValueFunction> operator()(const M & model);

        private:
            unsigned horizon_;
            double epsilon_;

            using SupportSet = std::unordered_set<VEntry, boost::hash<VEntry>>;
            struct Vertex;

            struct VertexComparator {
                bool operator() (const Vertex & lhs, const Vertex & rhs) const;
            };

            struct Vertex {
                Belief belief;
                SupportSet::iterator support;
                double currentValue;
                double error;
            };

            // This is our priority queue with all the vertices.
            using Agenda = boost::heap::fibonacci_heap<Vertex, boost::heap::compare<VertexComparator>>;

            Agenda agenda_;
    };

    template <typename M, typename>
    std::tuple<double, ValueFunction> LinearSupport::operator()(const M& model) {
        const auto S = model.getS();

        Projecter project(model);
        auto v = makeValueFunction(S); // TODO: May take user input

        unsigned timestep = 0;
        const bool useEpsilon = checkDifferentSmall(epsilon_, 0.0);
        double variation = epsilon_ * 2; // Make it bigger
        while ( timestep < horizon_ && ( !useEpsilon || variation > epsilon_ ) ) {
            ++timestep;

            auto projections = project(v[timestep-1]);

            // These are the good vectors, the ones that we are going to return for
            // sure.
            VList goodSupports;

            // We use this as a sorted linked list to handle all vectors, so it's
            // easy to check whether we already have one, and also each vertex can
            // keep references to them and we don't duplicate vectors all over the
            // place.
            SupportSet allSupports;

            std::vector<std::pair<Belief, double>> vertices;
            std::unordered_set<Belief, boost::hash<Belief>> triedVertices;

            // For each corner belief, find its value and alphavector. Add the
            // alphavectors in a separate list, remove duplicates. Note: In theory
            // we must be able to find all alphas for each corner, not just a
            // single best but all best. Cassandra does not do that though.. maybe
            // we can avoid it? He uses the more powerful corner detection tho.
            Belief corner(S); corner.fill(0.0);
            for (size_t s = 0; s < S; ++s) {
                corner[s] = 1.0;

                const auto [it, inserted] = allSupports.emplace(crossSumBestAtBelief(corner, projections));
                if (inserted) goodSupports.push_back(*it);

                corner[s] = 0.0;
            }

            // Now we find for all the alphavectors we have found, the vertices of
            // the polytope that they created. These vertices will bootstrap the
            // algorithm.
            auto goodBegin = boost::make_transform_iterator(goodSupports.cbegin(), unwrap);
            for (size_t i = 0; i < goodSupports.size(); ++i, ++goodBegin) {
                // For each alpha, we find its vertices against the others.
                IndexSkipMap map({i}, goodSupports);
                const auto cbegin = boost::make_transform_iterator(map.cbegin(), unwrap);
                const auto cend   = boost::make_transform_iterator(map.cend(), unwrap);

                // Note that the range we pass here is made by a single vector.
                auto newVertices = findVerticesNaive(goodBegin, goodBegin + 1, cbegin, cend);

                vertices.insert(std::end(vertices),
                        std::make_move_iterator(std::begin(newVertices)),
                        std::make_move_iterator(std::end(newVertices))
                );
            }

            do {
                // For each corner, we find its true alphas and its best possible value.
                // Then we compute the error between a corner's known true value and
                // what we can do with the optimal alphas we already have.
                // If the error is low enough, we don't need them. Otherwise we add
                // them to the priority queue.
                for (auto & vertex : vertices) {
                    if (triedVertices.find(vertex.first) != std::end(triedVertices))
                        continue;

                    double trueValue;
                    auto support = crossSumBestAtBelief(vertex.first, projections, &trueValue);

                    double currentValue = vertex.second;
                    // FIXME: As long as we use the naive way to find vertices,
                    // we can't really trust the values that come out as they
                    // may be lower than what we actually have. So we are
                    // forced to recompute their value.
                    const auto gsBegin = boost::make_transform_iterator(std::cbegin(goodSupports), unwrap);
                    const auto gsEnd   = boost::make_transform_iterator(std::cend(goodSupports),   unwrap);
                    findBestAtPoint(vertex.first, gsBegin, gsEnd, &currentValue);

                    auto diff = trueValue - currentValue;
                    if (diff > epsilon_ && checkDifferentGeneral(diff, epsilon_)) {
                        auto it = allSupports.insert(std::move(support));
                        Vertex newVertex;
                        newVertex.belief = vertex.first;
                        newVertex.currentValue = currentValue;
                        newVertex.support = it.first;
                        newVertex.error = diff;
                        agenda_.push(std::move(newVertex));
                    }

                    triedVertices.insert(std::move(vertex.first));
                }

                if (agenda_.size() == 0)
                    break;

                Vertex best = agenda_.top();
                agenda_.pop();

                AI_LOGGER(AI_SEVERITY_INFO, "Selected Vertex " << best.belief.transpose() << " as best, with support: " << best.support->values.transpose() << ", action: " << best.support->action);

                std::vector<Agenda::handle_type> verticesToRemove;

                // For each element in the agenda, we need to check whether any would
                // be made useless by the new supports that best is bringing in. If so,
                // we can remove them from the queue.
                for (auto it = agenda_.begin(); it != agenda_.end(); ++it) {
                    // If so, *their* supports, plus the supports of best form the surface
                    // in which we need to find vertices.
                    if (it->belief.dot(best.support->values) > it->currentValue)
                        verticesToRemove.push_back(Agenda::s_handle_from_iterator(it));
                }

                AI_LOGGER(AI_SEVERITY_DEBUG, "Removing " << verticesToRemove.size() << " vertices, as they are now obsolete.");
                for (auto & h : verticesToRemove) {
                    agenda_.erase(h);
                }

                // Find vertices between the best support of this belief and the list
                // we already have.
                const auto bestAddr = &(*best.support);
                const auto supBegin = boost::make_transform_iterator(bestAddr,                  unwrap);
                const auto supEnd   = boost::make_transform_iterator(bestAddr + 1,              unwrap);
                const auto chkBegin = boost::make_transform_iterator(std::cbegin(goodSupports), unwrap);
                const auto chkEnd   = boost::make_transform_iterator(std::cend(goodSupports),   unwrap);
                vertices = findVerticesNaive(supBegin, supEnd, chkBegin, chkEnd);

                // We now can add the support for this vertex to the main list.  We
                // don't need checks here because we are guaranteed that we are
                // improving the VList.
                goodSupports.push_back(*best.support);
            }
            while (true);

            v.emplace_back(std::move(goodSupports));

            // Check convergence
            if ( useEpsilon ) {
                variation = weakBoundDistance(v[timestep-1], v[timestep]);
            }
        }

        return std::make_tuple(useEpsilon ? variation : 0.0, v);
    }
}

#endif