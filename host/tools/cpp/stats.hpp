#pragma once

#include <fades/stream_device.hpp>

#include <iterator>
#include <ostream>

inline void print_stats(std::ostream &out, const fades::SolverStats &stats) {
    out << "HW statistics:\n"
        << "  accepted: " << stats.accepted << '\n'
        << "  dispatched: " << stats.dispatched << '\n'
        << "  collected: " << stats.collected << '\n'
        << "  emitted: " << stats.emitted << '\n'
        << "  processing_cycles: " << stats.processing_cycles << '\n'
        << "  dispatch_wait_cycles: " << stats.dispatch_wait_cycles << '\n';
    for (unsigned i = 0; i < std::size(stats.core_dispatched); ++i) {
        out << "Core " << i << ":\n"
            << "  received: " << stats.core_dispatched[i] << '\n'
            << "  output: " << stats.core_collected[i] << '\n'
            << "  busy_cycles: " << stats.core_busy_cycles[i] << '\n'
            << "  output_wait_cycles: " << stats.core_output_wait_cycles[i] << '\n'
            << "  input_wait_cycles: " << stats.core_input_wait_cycles[i] << '\n'
            << "  queue_peak: " << stats.core_queue_peak[i] << '\n'
            << "  branches: " << stats.core_branches[i] << '\n'
            << "  max_pending: " << stats.core_max_pending[i] << '\n'
            << "  evaluated: " << stats.core_evaluated[i] << '\n'
            << "  branches_per_problem: ";
        if (stats.core_collected[i])
            out << double(stats.core_branches[i]) / stats.core_collected[i];
        else
            out << "N/A";
        out << "\n  busy_cycles_per_state: ";
        if (stats.core_evaluated[i])
            out << double(stats.core_busy_cycles[i]) / stats.core_evaluated[i];
        else
            out << "N/A";
        out << '\n';
    }
}
