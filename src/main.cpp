// This file is part of PRSice2.0, copyright (C) 2016-2017
// Shing Wan Choi, Jack Euesden, Cathryn M. Lewis, Paul F. O’Reilly
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "plink_common.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "commander.hpp"
#include "genotype.hpp"
#include "genotypefactory.hpp"
#include "prsice.hpp"
#include "region.hpp"
#include "reporter.hpp"
int main(int argc, char* argv[])
{
    Reporter reporter;
    try
    {
        Commander commander;
        try
        {
            if (!commander.init(argc, argv, reporter))
                return 0; // only require the usage information
        }
        catch (const std::runtime_error& error)
        {
            return -1; // all error messages should have printed
        }
        bool verbose = true;
        // this allow us to generate the appropriate object (i.e. binaryplink /
        // binarygen)
        Region exclusion(commander.exclusion_range(), reporter);
        GenomeFactory factory;
        Genotype *target_file, *reference_file;
        try
        {
            target_file = factory.createGenotype(
                commander.target_name(), commander.target_type(),
                commander.target_list(), commander.thread(),
                commander.ignore_fid(), commander.nonfounders(),
                commander.keep_ambig(), reporter, commander);
            target_file->load_samples(commander.keep_sample_file(),
                                      commander.remove_sample_file(), verbose,
                                      reporter);
            if (commander.use_ref()) target_file->expect_reference();
            target_file->load_snps(
                commander.out(), commander.extract_file(),
                commander.exclude_file(), commander.geno(), commander.maf(),
                commander.info(), commander.hard_threshold(),
                commander.hard_coded(), exclusion, verbose, reporter);
        }
        catch (const std::invalid_argument& ia)
        {
            reporter.report(ia.what());
            return -1;
        }
        catch (const std::runtime_error& error)
        {
            reporter.report(error.what());
            return -1;
        }
        // TODO: Revamp Region to make it suitable for prslice too
        Region region(commander.feature(), commander.window_5(),
                      commander.window_3());
        try
        {
            region.run(commander.gtf(), commander.msigdb(), commander.bed(),
                       commander.single_snp_set(), commander.multi_snp_sets(),
                       *target_file, commander.out(), commander.background(),
                       reporter);
        }
        catch (const std::runtime_error& error)
        {
            reporter.report(error.what());
            return -1;
        }

        // Might want to generate a log file?
        region.info(reporter);

        bool perform_prslice = commander.perform_prslice();

        // Need to handle paths in the name
        std::string base_name = misc::remove_extension<std::string>(
            misc::base_name<std::string>(commander.base_name()));
        try
        {
            target_file->set_info(commander);
            // load reference panel first so that we have updated the target

            std::string message = "Start processing " + base_name + "\n";
            message.append("==============================\n");
            reporter.report(message);
            target_file->read_base(commander, region, reporter);


            // we no longer need the region boundaries
            // as we don't allow multiple base file input
            region.clean();
            // TODO: This is no longer useful and can be deleted in next
            // release, once Yunfeng has completed her analysis
            // std::string region_out_name = commander.out() + ".region";
            // output the number of SNPs observed in each sets
            // region.print_file(region_out_name);
            // initialize PRSice class
            PRSice prsice(base_name, commander, region.size() > 1,
                          target_file->num_sample(), reporter);
            // check the phenotype input columns
            prsice.pheno_check(commander, reporter);

            // perform clumping (Main problem with memory here)
            if (!commander.no_clump()) {
                if (commander.use_ref()) {
                    reporter.report("Loading reference "
                                    "panel\n==============================\n");
                    reference_file = factory.createGenotype(
                        commander.ref_name(), commander.ref_type(),
                        commander.ref_list(), commander.thread(),
                        commander.ignore_fid(), commander.nonfounders(),
                        commander.keep_ambig(), reporter, commander, true);

                    reference_file->load_samples(commander.ld_keep_file(),
                                                 commander.ld_remove_file(),
                                                 verbose, reporter);
                    // only load SNPs that can be found in the target file index
                    reference_file->load_snps(
                        commander.out(), commander.extract_file(),
                        commander.exclude_file(), commander.geno(),
                        commander.maf(), commander.info(),
                        commander.hard_threshold(), commander.hard_coded(),
                        exclusion, verbose, reporter, target_file);
                }
                // get the sort by p index vector for target
                // so that we can still find out the relative coordinates of
                // each SNPs This is only required for clumping
                if (!target_file->sort_by_p()) {
                    std::string error_message =
                        "No SNPs left for PRSice processing";
                    reporter.report(error_message);
                    return -1;
                }
                target_file->efficient_clumping(
                    commander.use_ref() ? *reference_file : *target_file,
                    reporter, commander.pearson());
                // immediately free the memory if needed
                if (commander.use_ref()) delete reference_file;
            }
            if (!target_file->prepare_prsice(reporter)) {
                std::string error_message =
                    "No SNPs left for PRSice processing";
                reporter.report(error_message);
                return -1;
            }
            target_file->count_snp_in_region(region, commander.out(),
                                             commander.print_snp());

            // check which region are removed
            std::ofstream removed_regions;
            size_t region_size = region.size();
            for (size_t i = 0; i < region_size; ++i) {
                if (region.num_post_clump_snp(i) == 0) {
                    // this is not included in the analysis
                    if (!removed_regions.is_open()) {
                        removed_regions.open(
                            std::string(commander.out() + ".excluded_regions")
                                .c_str());
                        if (!removed_regions.is_open()) {
                            fprintf(stderr,
                                    "Error: Cannot open file to write: %s\n",
                                    std::string(commander.out()
                                                + ".excluded_regions")
                                        .c_str());
                            return -1;
                        }
                    }
                    removed_regions << region.get_name(i) << std::endl;
                }
            }
            if (removed_regions.is_open()) removed_regions.close();
            const size_t num_pheno = prsice.num_phenotype();
            if (!perform_prslice) {
                // this count is specific for PRSice
                prsice.init_process_count(commander, region.size(),
                                          target_file->num_threshold());
                const size_t num_region_process =
                    region.size() - (region.size() > 1 ? 1 : 0);
                for (size_t i_pheno = 0; i_pheno < num_pheno; ++i_pheno) {
                    // initialize the phenotype & independent variable matrix
                    fprintf(stderr, "\nProcessing the %zu th phenotype\n",
                            i_pheno + 1);
                    prsice.init_matrix(commander, i_pheno, *target_file,
                                       reporter);
                    prsice.prep_output(commander, *target_file, region.names(),
                                       i_pheno);
                    // go through each region separately
                    // this should reduce the memory usage
                    for (size_t i_region = 0; i_region < num_region_process;
                         ++i_region)
                    {
                        if (region.num_post_clump_snp(i_region) == 0) continue;
                        prsice.run_prsice(commander, region, i_pheno, i_region,
                                          *target_file);
                        if (!commander.no_regress())
                            prsice.output(commander, region, i_pheno, i_region,
                                          *target_file);
                    }
                    if (!commander.no_regress() && commander.perform_set_perm())
                    {
                        prsice.run_competitive(*target_file, commander,
                                               i_pheno);
                    }
                }
                prsice.print_progress(true);
                fprintf(stderr, "\n");
                if (!commander.no_regress())
                    prsice.summarize(commander, reporter);
            }
            else
            {
                std::string error_message =
                    "Error: We currently have not implemented PRSlice. We will "
                    "implement PRSlice once the implementation of PRSice is "
                    "stabalized";
                reporter.report(error_message);
                return -1;
            }
        }
        catch (const std::out_of_range& error)
        {
            reporter.report(error.what());
            return -1;
        }
        catch (const std::runtime_error& error)
        {
            reporter.report(error.what());
            return -1;
        }
        delete target_file;
    }
    catch (const std::bad_alloc& er)
    {
        std::string error_message = "Error: Bad Allocation exception detected. "
                                    "This is likely due to insufficient memory "
                                    "for PRSice. You can try re-running PRSice "
                                    "with more memory.";
        reporter.report(error_message);
    }
    catch (const std::exception& ex)
    {
        reporter.report(ex.what());
    }
    return 0;
}
