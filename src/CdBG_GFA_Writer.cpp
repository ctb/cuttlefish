
#include "CdBG.hpp"
#include "kseq/kseq.h"

#include <chrono>
#include <thread>
#include "zlib.h"


// Declare the type of file handler and the read() function.
// Required for FASTA/FASTQ file reading using the kseq library.
KSEQ_INIT(int, read);


void CdBG::output_maximal_unitigs_gfa(const std::string& gfa_file_name, const uint16_t thread_count)
{
    std::chrono::high_resolution_clock::time_point t_start = std::chrono::high_resolution_clock::now();


    // Open the file handler for the FASTA / FASTQ file containing the reference.
    FILE* input = fopen(ref_file.c_str(), "r");
    if(input == NULL)
    {
        std::cerr << "Error opening input file " << ref_file << ". Aborting.\n";
        std::exit(EXIT_FAILURE);
    }

    // Initialize the parser.
    kseq_t* parser = kseq_init(fileno(input));


    // Clear the output file.
    std::ofstream op_stream(gfa_file_name.c_str(), std::ofstream::out);
    if(!op_stream)
    {
        std::cerr << "Error opening output file " << gfa_file_name << ". Aborting.\n";
        std::exit(EXIT_FAILURE);
    }

    // Write the GFA header.
    op_stream << GFA_HEADER << "\n";
    op_stream.close();


    // Allocate the output buffers for each thread.
    output_buffer.resize(thread_count);
    buffer_size.resize(thread_count);

    // Allocate entries for the first, the second, and the last unitigs seen by each thread.
    first_unitig.resize(thread_count);
    second_unitig.resize(thread_count);
    last_unitig.resize(thread_count);


    // Parse sequences one-by-one, and output each unique maximal unitig encountered through them.
    // uint32_t seq_count = 0;
    seq_count = 0;
    while(kseq_read(parser) >= 0)
    {
        const char* seq = parser->seq.s;
        const size_t seq_len = parser->seq.l;

        std::chrono::high_resolution_clock::time_point t_s = std::chrono::high_resolution_clock::now();
        std::cout << "Processing sequence " << ++seq_count << ", with length " << seq_len << ".\n";

        // Nothing to process for sequences with length shorter than `k`.
        if(seq_len < k)
            continue;


        // Open an asynchronous logger to write into the output file, and set its log message pattern.
        // Note: `spdlog` appends to the output file by default, so the results for the sequences are accumulated into the same output file.
        cuttlefish::logger_t output = spdlog::basic_logger_mt<spdlog::async_factory>("async_file_logger", gfa_file_name.c_str());
        output->set_pattern("%v");

        
        // Reset the first, the second, and the last unitigs seen for each thread.
        std::fill(first_unitig.begin(), first_unitig.end(), Oriented_Unitig());
        std::fill(second_unitig.begin(), second_unitig.end(), Oriented_Unitig());
        std::fill(last_unitig.begin(), last_unitig.end(), Oriented_Unitig());

        // Reset the path output streams for each thread.
        reset_path_streams(thread_count);
        

        // Single-threaded writing.
        // output_off_substring(seq, seq_len, 0, seq_len - k, output);

        // Multi-threaded writing.
        size_t task_size = (seq_len - k + 1) / thread_count;
        if(!task_size)
            output_gfa_off_substring(0, seq, seq_len, 0, seq_len - k, output);
        else
        {
            std::vector<std::thread> task;
            size_t left_end = 0;
            size_t right_end;

            for(uint16_t task_id = 0; task_id < thread_count; ++task_id)
            {
                right_end = (task_id == thread_count - 1 ? seq_len - k : left_end + task_size - 1);
                task.emplace_back(&CdBG::output_gfa_off_substring, this, static_cast<uint64_t>(task_id), seq, seq_len, left_end, right_end, output);
                left_end += task_size;
            }

            for(std::thread& t: task)
                if(t.joinable())
                    t.join();
                else
                {
                    std::cerr << "Early termination of a worker thread encountered during writing of maximal unitigs. Aborting.\n";
                    std::exit(EXIT_FAILURE);
                }


            write_inter_thread_links(thread_count, output);

            std::chrono::high_resolution_clock::time_point t_e = std::chrono::high_resolution_clock::now();
            double elapsed_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(t_e - t_s).count();
            // std::cout << "Time taken to write segments and links = " << elapsed_seconds << " seconds.\n";
        }
        
        // Flush all the buffered content (segments and links), as the GFA path to be appended to
        // the same output sink file is written in a different way than using the `spdlog` logger.
        // Note: If using an async logger, `logger->flush()` posts a message to the queue requesting the flush
        // operation, so the function returns immediately. Hence a forceful eviction is necessary by shutdown.
        output->flush();
        spdlog::shutdown();
        

        // Write the GFA path for this sequence.
        write_gfa_path(thread_count, gfa_file_name);
    }


    // Flush the buffers.
    cuttlefish::logger_t output = spdlog::basic_logger_mt<spdlog::async_factory>("async_file_logger", gfa_file_name.c_str());
    output->set_pattern("%v");
    flush_buffers(thread_count, output);

    // Remove the temporary files.
    remove_temp_files(thread_count);

    
    // Close the loggers?
    spdlog::drop_all();


    // Close the parser and the input file.
    kseq_destroy(parser);
    fclose(input);


    std::chrono::high_resolution_clock::time_point t_end = std::chrono::high_resolution_clock::now();
    double elapsed_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(t_end - t_start).count();
    std::cout << "Done outputting the maximal unitigs. Time taken = " << elapsed_seconds << " seconds.\n";
}


void CdBG::reset_path_streams(const uint16_t thread_count)
{
    path_output.clear();
    overlap_output.clear();

    for(uint16_t t_id = 0; t_id < thread_count; ++t_id)
    {
        path_output.emplace_back((PATH_OUTPUT_PREFIX + std::to_string(t_id)).c_str(), std::ofstream::out);
        overlap_output.emplace_back((OVERLAP_OUTPUT_PREFIX + std::to_string(t_id)).c_str(), std::ofstream::out);

        if(path_output[t_id].fail() || overlap_output[t_id].fail())
        {
            std::cerr << "Error opening temporary path output files. Aborting.\n";
            std::exit(EXIT_FAILURE);
        }
    }
}


void CdBG::output_gfa_off_substring(const uint64_t thread_id, const char* seq, const size_t seq_len, const size_t left_end, const size_t right_end, cuttlefish::logger_t output)
{
    size_t kmer_idx = left_end;
    while(kmer_idx <= right_end)
    {
        kmer_idx = search_valid_kmer(seq, kmer_idx, right_end);

        // No valid k-mer remains in the sequence.
        if(kmer_idx > right_end)
            break;

        // Process a maximal valid contiguous subsequence, and advance to the index following it.
        kmer_idx = output_maximal_unitigs_gfa(thread_id, seq, seq_len, right_end, kmer_idx, output);
    }
}


size_t CdBG::output_maximal_unitigs_gfa(const uint64_t thread_id, const char* seq, const size_t seq_len, const size_t right_end, const size_t start_idx, cuttlefish::logger_t output)
{
    size_t kmer_idx = start_idx;

    // assert(kmer_idx <= seq_len - k);

    Annotated_Kmer curr_kmer(cuttlefish::kmer_t(seq, kmer_idx), kmer_idx, Vertices);

    // The subsequence contains only an isolated k-mer, i.e. there's no valid left or right
    // neighboring k-mer to this k-mer. So it's a maximal unitig by itself.
    if((kmer_idx == 0 || seq[kmer_idx - 1] == cuttlefish::PLACEHOLDER_NUCLEOTIDE) &&
        (kmer_idx + k == seq_len || seq[kmer_idx + k] == cuttlefish::PLACEHOLDER_NUCLEOTIDE))
        output_unitig_gfa(thread_id, seq, curr_kmer, curr_kmer, output);
    else    // At least one valid neighbor exists, either to the left or to the right, or on both sides.
    {
        // No valid right neighbor exists for the k-mer.
        if(kmer_idx + k == seq_len || seq[kmer_idx + k] == cuttlefish::PLACEHOLDER_NUCLEOTIDE)
        {
            // A valid left neighbor exists as it's not an isolated k-mer.
            Annotated_Kmer prev_kmer(cuttlefish::kmer_t(seq, kmer_idx - 1), kmer_idx, Vertices);
            
            if(is_unipath_start(curr_kmer.vertex_class, curr_kmer.dir, prev_kmer.vertex_class, prev_kmer.dir))
                // A maximal unitig ends at the ending of a maximal valid subsequence.
                output_unitig_gfa(thread_id, seq, curr_kmer, curr_kmer, output);

            // The contiguous sequence ends at this k-mer.
            return kmer_idx + k;
        }


        // A valid right neighbor exists for the k-mer.
        Annotated_Kmer next_kmer = curr_kmer;
        next_kmer.roll_to_next_kmer(seq[kmer_idx + k], Vertices);

        bool on_unipath = false;
        Annotated_Kmer unipath_start_kmer;
        Annotated_Kmer prev_kmer;

        // No valid left neighbor exists for the k-mer.
        if(kmer_idx == 0 || seq[kmer_idx - 1] == cuttlefish::PLACEHOLDER_NUCLEOTIDE)
        {
            // A maximal unitig starts at the beginning of a maximal valid subsequence.
            on_unipath = true;
            unipath_start_kmer = curr_kmer;
        }
        // Both left and right valid neighbors exist for this k-mer.
        else
        {
            prev_kmer = Annotated_Kmer(cuttlefish::kmer_t(seq, kmer_idx - 1), kmer_idx, Vertices);
            if(is_unipath_start(curr_kmer.vertex_class, curr_kmer.dir, prev_kmer.vertex_class, prev_kmer.dir))
            {
                on_unipath = true;
                unipath_start_kmer = curr_kmer;
            }
        }

        if(on_unipath && is_unipath_end(curr_kmer.vertex_class, curr_kmer.dir, next_kmer.vertex_class, next_kmer.dir))
        {
            output_unitig_gfa(thread_id, seq, unipath_start_kmer, curr_kmer, output);
            on_unipath = false;
        }


        // Process the rest of the k-mers of this contiguous subsequence.
        for(kmer_idx++; on_unipath || kmer_idx <= right_end; ++kmer_idx)
        {
            prev_kmer = curr_kmer;
            curr_kmer = next_kmer;

            if(is_unipath_start(curr_kmer.vertex_class, curr_kmer.dir, prev_kmer.vertex_class, prev_kmer.dir))
            {
                on_unipath = true;
                unipath_start_kmer = curr_kmer;
            }


            // No valid right neighbor exists for the k-mer.
            if(kmer_idx + k == seq_len || seq[kmer_idx + k] == cuttlefish::PLACEHOLDER_NUCLEOTIDE)
            {
                // A maximal unitig ends at the ending of a maximal valid subsequence.
                if(on_unipath)
                {
                    output_unitig_gfa(thread_id, seq, unipath_start_kmer, curr_kmer, output);
                    on_unipath = false;
                }

                // The contiguous sequence ends at this k-mer.
                return kmer_idx + k;
            }
            else    // A valid right neighbor exists.
            {
                next_kmer.roll_to_next_kmer(seq[kmer_idx + k], Vertices);
                
                if(on_unipath && is_unipath_end(curr_kmer.vertex_class, curr_kmer.dir, next_kmer.vertex_class, next_kmer.dir))
                {
                    output_unitig_gfa(thread_id, seq, unipath_start_kmer, curr_kmer, output);
                    on_unipath = false;
                }
            }
        }
    }
    
    
    // Return the non-inclusive ending index of the processed contiguous subsequence.
    return kmer_idx + k;
}


void CdBG::output_unitig_gfa(const uint64_t thread_id, const char* seq, const Annotated_Kmer& start_kmer, const Annotated_Kmer& end_kmer, cuttlefish::logger_t output)
{
    // This is to avoid race conditions that may arise while multi-threading.
    // If two threads try to output the same unitig at the same time but
    // encounter it in the opposite orientations, then data races may arise.
    // For a particular unitig, always query the same well-defined canonical flanking
    // k-mer, irrespective of which direction the unitig may be traversed at.
    const cuttlefish::kmer_t min_flanking_kmer = std::min(start_kmer.canonical, end_kmer.canonical);
    const uint64_t bucket_id = Vertices.bucket_id(min_flanking_kmer);
    Kmer_Hash_Entry_API hash_table_entry = Vertices[bucket_id];
    State& state = hash_table_entry.get_state();

    // Name the GFA segment with the hash value of the first k-mer of the canonical form unitig.
    const uint64_t unitig_id = bucket_id;
    const cuttlefish::kmer_dir_t unitig_dir = (start_kmer.kmer < end_kmer.rev_compl ? cuttlefish::FWD : cuttlefish::BWD);
    const Oriented_Unitig current_unitig(unitig_id, unitig_dir, start_kmer.idx, end_kmer.idx);


    // Output a possible GFA segment.

    if(!state.is_outputted())
    {
        state = state.outputted();

        // If the hash table update is successful, only then this thread may output this unitig.
        if(Vertices.update(hash_table_entry))
            write_gfa_segment(thread_id, seq, unitig_id, start_kmer.idx, end_kmer.idx, unitig_dir, output);
    }


    // Output a possible GFA link.

    if(!first_unitig[thread_id].is_valid())
        first_unitig[thread_id] = current_unitig;
    else if(!second_unitig[thread_id].is_valid())
        second_unitig[thread_id] = current_unitig;

    Oriented_Unitig& prev_unitig = last_unitig[thread_id];
    if(prev_unitig.is_valid())
    {
        write_gfa_link(thread_id, prev_unitig, current_unitig, output);

        append_link_to_path(thread_id, prev_unitig, current_unitig);
    }
    
    prev_unitig = current_unitig;
}


void CdBG::write_gfa_segment(const uint64_t thread_id, const char* seq, const uint64_t segment_name, const size_t start_kmer_idx, const size_t end_kmer_idx, const cuttlefish::kmer_dir_t dir, cuttlefish::logger_t output) 
{
    std::stringstream& buffer = output_buffer[thread_id];

    // Write the 'RecordType' and 'Name' fields for the segment line.
    buffer << "S\t" << segment_name;
    
    // Write the segment field.
    buffer << "\t";
    if(dir == cuttlefish::FWD)
        for(size_t idx = start_kmer_idx; idx <= end_kmer_idx + k - 1; ++idx)
            buffer << seq[idx];
    else
    {
        // To avoid underflow of unsigned integers, the flanking indices are incremented by 1.
        size_t idx = end_kmer_idx + k;
        while(idx > start_kmer_idx)
        {
            idx--;
            buffer << complement(seq[idx]);
        }
    }


    // Write some optional fields that are trivially inferrable here.
    buffer << "\tLN:i:" << (end_kmer_idx - start_kmer_idx + k); // The segment length.
    buffer << "\tKC:i:" << (end_kmer_idx - start_kmer_idx + 1); // The k-mer count.


    // End the segment line.
    buffer << "\n";


    // Mark buffer size increment.
    fill_buffer(thread_id, 1, output);
}


void CdBG::write_gfa_link(const uint64_t thread_id, const Oriented_Unitig& left_unitig, const Oriented_Unitig& right_unitig, cuttlefish::logger_t output)
{
    std::stringstream& buffer = output_buffer[thread_id];

    // Write the 'RecordType' field for the link line.
    buffer << "L";

    // Write the 'From' fields.
    buffer << "\t" << left_unitig.unitig_id << "\t" << (left_unitig.dir == cuttlefish::FWD ? "+" : "-");

    // Write the 'To' fields.
    buffer << "\t" << right_unitig.unitig_id << "\t" << (right_unitig.dir == cuttlefish::FWD ? "+" : "-");

    // Write the 'Overlap' field.
    const uint16_t overlap = (right_unitig.start_kmer_idx == left_unitig.end_kmer_idx + 1 ? k - 1 : 0);
    buffer << "\t" << overlap << "M";


    // End the link line.
    buffer << "\n";


    // Mark buffer size increment.
    fill_buffer(thread_id, 1, output);
}


void CdBG::append_link_to_path(const uint64_t thread_id, const Oriented_Unitig& left_unitig, const Oriented_Unitig& right_unitig)
{
    // The destination vertex (unitig) is written for each link.
    // Note that, the very first vertex of the path tiling for the sequence is thus missing in the path outputs.

    path_output[thread_id] << "," << right_unitig.unitig_id << (right_unitig.dir == cuttlefish::FWD ? "+" : "-");
    overlap_output[thread_id] << "," << (right_unitig.start_kmer_idx == left_unitig.end_kmer_idx + 1 ? k - 1 : 0) << "M";

    // Error checking for writing failures is done while closing the streams.
}


void CdBG::write_inter_thread_links(const uint16_t thread_count, cuttlefish::logger_t output)
{
    Oriented_Unitig left_unitig;
    uint16_t left_t_id;

    for(uint16_t t_id = 0; t_id < thread_count; ++t_id)
        if(!left_unitig.is_valid())
        {
            left_unitig = last_unitig[t_id];
            left_t_id = t_id;
        }
        else
            if(first_unitig[t_id].is_valid())
            {
                // A link exists between the last unitig of the thread number `left_t_id`
                // and the first unitig of the thread number `t_id`.
                const Oriented_Unitig& right_unitig = first_unitig[t_id];
                write_gfa_link(t_id, left_unitig, right_unitig, output);
                append_link_to_path(left_t_id, left_unitig, right_unitig);

                // There definitely exists a last unitig for the thread number `t_id`, as it has a first unitig.
                left_unitig = last_unitig[t_id];
                left_t_id = t_id;
            }
}


void CdBG::search_first_link(const uint16_t thread_count, Oriented_Unitig& left_unitig, Oriented_Unitig& right_unitig) const
{
    left_unitig = Oriented_Unitig();
    right_unitig = Oriented_Unitig();

    for(uint16_t t_id = 0; t_id < thread_count; ++t_id)
    {
        if(first_unitig[t_id].is_valid())
        {
            if(!left_unitig.is_valid())
                left_unitig = first_unitig[t_id];
            else
            {
                right_unitig = first_unitig[t_id];
                return;
            }
        }

        if(second_unitig[t_id].is_valid())
        {
            // Obviously, `first_unitig[t_id]` must also be valid for the thread number `t_id`;
            // so `left_unitig` is already set to a valid value at this point.
            right_unitig = second_unitig[t_id];
            return;
        }
    }
}


void CdBG::write_gfa_path(const uint16_t thread_count, const std::string& gfa_file_name)
{
    std::chrono::high_resolution_clock::time_point t_start = std::chrono::high_resolution_clock::now();

    // Close the path output streams.
    for(uint16_t t_id = 0; t_id < thread_count; ++t_id)
    {
        if(path_output[t_id].fail() || overlap_output[t_id].fail())
        {
            std::cerr << "Errors encountered while writing to the temporary path output files. Aborting.\n";
            std::exit(EXIT_FAILURE);
        }

        path_output[t_id].close();
        overlap_output[t_id].close();
    }
    

    // Search the very first GFA link in the sequence, as that is not inferrable from the path outputs.
    Oriented_Unitig left_unitig;
    Oriented_Unitig right_unitig;
    search_first_link(thread_count, left_unitig, right_unitig);

    // The sequence does not contain any unitig (possible if there's no valid k-mer in the sequence).
    if(!left_unitig.is_valid())
        return;

    
    // Open the output file in append mode.
    std::ofstream output(gfa_file_name.c_str(), std::ios_base::app);

    // Write the 'RecordType' field for the path line.
    output << "P";

    // Write the `PathName` field.
    output << "\tP" << seq_count;

    // Write the `SegmentNames` field.
    output << "\t";
    
    // The first vertex of the path (not inferrable from the path output files).
    output << left_unitig.unitig_id << (left_unitig.dir == cuttlefish::FWD ? "+" : "-");

    // Copy the thread-specific path output file contents to the GFA output file.
    for(uint16_t t_id = 0; t_id < thread_count; ++t_id)
    {
        const std::string path_file_name = (PATH_OUTPUT_PREFIX + std::to_string(t_id));
        std::ifstream input(path_file_name.c_str(), std::ios_base::in);

        if(input.fail())
        {
            std::cerr << "Error opening temporary path output file " << path_file_name << ". Aborting.\n";
            std::exit(EXIT_FAILURE);
        }
        
        // Copy the path output for thread number `t_id` to the end of the output GFA file.
        if(input.peek() != EOF)
            output << input.rdbuf();

        input.close();
    }


    // Write the `Overlaps` field.
    output << "\t";

    // The sequence contains only one unitig.
    if(!right_unitig.is_valid())
        output << "*";  // Write an empty CIGAR string at the 'Overlaps' field.
    else
    {
        // The first overlap of the path (not inferrable from the path output files).
        const uint16_t overlap = (right_unitig.start_kmer_idx == left_unitig.end_kmer_idx +  1 ? k - 1 : 0);
        output << overlap << "M";

        // Copy the thread-specific overlap output file contents to the GFA output file.
        for(uint16_t t_id = 0; t_id < thread_count; ++t_id)
        {
            const std::string overlap_file_name = (OVERLAP_OUTPUT_PREFIX + std::to_string(t_id));
            std::ifstream input(overlap_file_name.c_str(), std::ifstream::in);

            if(input.fail())
            {
                std::cerr << "Error opening temporary path output file " << overlap_file_name << ". Aborting.\n";
                std::exit(EXIT_FAILURE);
            }

            // Copy the overlaps output for thread number `t_id` to the end of the output GFA file.
            if(input.peek() != EOF)
                output << input.rdbuf();

            input.close();
        }
    }


    // End the path line.
    output << "\n";

    output.close();

    std::chrono::high_resolution_clock::time_point t_end = std::chrono::high_resolution_clock::now();
    double elapsed_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(t_end - t_start).count();
    // std::cout << "Time taken to write paths = " << elapsed_seconds << " seconds.\n";
}


void CdBG::remove_temp_files(const uint16_t thread_count) const
{
    for(uint16_t t_id = 0; t_id < thread_count; ++t_id)
    {
        const std::string path_file_name = PATH_OUTPUT_PREFIX + std::to_string(t_id);
        const std::string overlap_file_name = OVERLAP_OUTPUT_PREFIX + std::to_string(t_id);

        if(remove(path_file_name.c_str()) != 0 || remove(overlap_file_name.c_str()) != 0)
        {
            std::cerr << "Error deleting temporary files.\n";
        }
    }
}
