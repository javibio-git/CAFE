/*! \page ErrorModel Error Model
* \code{.sh}
# errormodel [-model filename] [-sp species name | -all ] 
* \endcode
The \b errormodel command allows the user to specify an error distribution. CAFE will correct for this error before 
calculating ancestral family sizes and estimating &lambda; values. The errormodel function is also used by caferror.py to 
estimate error in the input data set.

-model <em>error model file</em>: This option allows the user to specify the errorfile to use in order to correct 
the input data for errors. The error model file format should be as follows:

\code
maxcnt: 68
cntdiff -1 0 1
0 0.0 0.8 0.2
1 0.2 0.6 0.2
2 0.2 0.6 0.2
...
68 0.2 0.6 0.2
\endcode

In this file, \b maxcnt is the largest family size observed in the dataset. Errorclasses (for all following rows) are 
defined with \b cntdiff and act as labels for error distributions for each gene family size. Error classes must be 
space-delimited positive or negative integers (and 0). The error class with label 0 means that this corresponds to no 
change in gene family size due to error. After the first two lines, each possible family size in the dataset (size 0 to 
\b maxcnt ) should have an error distribution defined. Any omitted family size follows the distribution for the previous 
row. The error distribution for each count should be space delimited probabilities whose columns correspond to the 
error classes defined in line two. 

<em>Default</em>: No error model is applied.

\note 
1. You should not specify any negative error correction for family size of 0 as this cannot occur (i.e., there 
can't be negative gene family sizes); 
2. The rows of the error model file must sum to 1; 
3. If any gene counts are missing from the error model file, CAFE will assume the same error distribution from the 
previous line. This can also be used as a shortcut if you know that all of the gene counts are specified with the
same error distribution: simply enter the first four lines (\b maxcnt, \b cntdiff, <em>family size</em>=0,1) into 
the error model file and CAFE will use the distribution for family size=1 as the distribution for all gene family sizes.

-sp: This option is required to specify the species to which the error model will be applied. Species names must be 
identical to those in the data file and the input tree. The user may specify any combination of species with the same 
or different error model files with separate errormodel commands, or the user may specify all species with the same 
error model file in one errormodel command using -all as the species option here.

-all (see above)
*/

#include <cctype>
#include <algorithm>
#include <vector>
#include <sstream>
#include <cfloat>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <cmath>

#include "error_model.h"
#include "cafe_commands.h"
#include "gene_family.h"

extern "C" {
#include "cafe_shell.h"
    int __check_error_model_columnsums(pErrorStruct errormodel);
    void cafe_shell_free_errorstruct(pErrorStruct errormodel);
    int cafe_shell_read_error_true_measure(const char* errorfile, const char* truefile, int** observed_pairs, int maxFamilySize);
    double __loglikelihood_pairs_from_true_measure(double* parameters, void* args);
    pErrorStruct cafe_shell_create_error_matrix_from_estimate(pErrorMeasure errormeasure);
}

pErrorStruct cafe_shell_create_error_matrix_from_estimate(pErrorMeasure errormeasure);

struct to_lower {
    int operator() (int ch)
    {
        return std::tolower(ch);
    }
};

bool case_insensitive_equal(std::string s1, std::string s2)
{
    std::transform(s1.begin(), s1.end(), s1.begin(), to_lower());
    std::transform(s2.begin(), s2.end(), s2.begin(), to_lower());
    return s1 == s2;
}

std::vector<std::string> split(std::string str, char delimiter)
{
    std::vector<std::string> result;
    std::stringstream ss(str); // Turn the string into a stream.
    std::string tok;

    while (getline(ss, tok, delimiter)) {
        result.push_back(tok);
    }

    return result;
}

pErrorStruct get_error_model(pCafeFamily family, std::string filename)
{
    pErrorStruct result = NULL;
    if (family->errors) {
        for (int i = 0; i<family->errors->size; i++) {
            pErrorStruct error = (pErrorStruct)family->errors->array[i];
            if (case_insensitive_equal(error->errorfilename, filename)) {
                result = error;
                break;
            }
        }
    }

    return result;
}

std::ostream& operator<<(std::ostream& ost, ErrorStruct& errormodel)
{
    int i, j;

    ost << "maxcnt:" << errormodel.maxfamilysize << "\n";
    ost << "cntdiff";
    for (j = errormodel.fromdiff; j <= errormodel.todiff; j++) {
        ost << " " << j;
    }
    ost << "\n";

    ost << std::setw(2) << std::setprecision(2);
    for (j = 0; j <= errormodel.maxfamilysize; j++) {
        ost << j;
        for (i = errormodel.fromdiff; i <= errormodel.todiff; i++) {
            if (0 <= i + j && i + j <= errormodel.maxfamilysize) {
                ost << " " << errormodel.errormatrix[i + j][j]; // conditional probability of measuring i+j when true count is j
            }
            else {
                ost << " #nan";
            }
        }
        ost << "\n";
    }

    return ost;
}

std::istream& operator>>(std::istream& ifst, ErrorStruct& errormodel)
{
    std::string line;
    if (!std::getline(ifst, line))
    {
        throw std::runtime_error("Empty file");
    }
    std::vector<std::string> data = split(line, ' ');
    std::vector<std::string> max = split(data.at(0), ':');
    int file_row_count = atoi(max[1].c_str());
    errormodel.maxfamilysize = std::max(errormodel.maxfamilysize, file_row_count);

    if (std::getline(ifst, line)) {
        std::vector<std::string> data = split(line, ' ');
        errormodel.fromdiff = atoi(data.at(1).c_str());
        errormodel.todiff = atoi(data.at(data.size() - 1).c_str());
    }
    errormodel.errormatrix = (double**)memory_new_2dim(errormodel.maxfamilysize + 1, errormodel.maxfamilysize + 1, sizeof(double));

    int i = 0;
    int j = 0;
    while (std::getline(ifst, line))
    {
        data = split(line, ' ');
        int col1 = atoi(data[0].c_str());
        if ((int)data.size() == (errormodel.todiff - errormodel.fromdiff) + 2) {
            while (j && j < col1) {
                // copy previous line's error model for missing lines.
                for (i = errormodel.fromdiff; i <= errormodel.todiff; i++) {

                    if (i + j >= 0 && i + j <= errormodel.maxfamilysize) {
                        errormodel.errormatrix[i + j][j] = errormodel.errormatrix[i + j - 1][j - 1];
                    }
                }
                i++;
            }
            // read error model and save in matrix row
            int k = 1;  // k is file column index
            for (i = errormodel.fromdiff; i <= errormodel.todiff; i++) {
                assert(j == col1);
                if (i + j >= 0 && i + j <= errormodel.maxfamilysize) {
                    errormodel.errormatrix[i + j][j] = atof(data[k].c_str());  // conditional probability of measuring i+j when true count is j
                }
                k++;
            }
            j++;
        }
    }
    while (j && j <= errormodel.maxfamilysize) {
        // copy previous line's error model for missing lines till the end of matrix.
        for (i = errormodel.fromdiff; i <= errormodel.todiff; i++) {
            if (i + j >= 0 && i + j <= errormodel.maxfamilysize) {
                errormodel.errormatrix[i + j][j] = errormodel.errormatrix[i + j - 1][j - 1]; // conditional probability of measuring i+j when true count is j
            }
        }
        j++;
    }

    return ifst;
}

void init_error_ptr(pCafeFamily family, pCafeTree pTree, pErrorStruct errormodel, std::string speciesname)
{
    if (family->error_ptr == NULL) {
        family->error_ptr = (pErrorStruct *)memory_new(family->num_species, sizeof(pErrorStruct));
    }
    if (!speciesname.empty()) {
        for (int i = 0; i<family->num_species; i++) {
            if (case_insensitive_equal(family->species[i], speciesname)) {
                family->error_ptr[i] = errormodel;
                pCafeNode pcnode = (pCafeNode)pTree->super.nlist->array[family->index[i]];
                pcnode->errormodel = errormodel;
                break;
            }
        }
    }
    else { // '-all' specified instead of speciesname
        for (int i = 0; i<family->num_species; i++) {
            family->error_ptr[i] = errormodel;
            pCafeNode pcnode = (pCafeNode)pTree->super.nlist->array[family->index[i]];
            pcnode->errormodel = errormodel;
        }
    }

}

int set_error_matrix_from_file(pCafeFamily family, pCafeTree pTree, family_size_range& range, std::string filename, std::string speciesname)
{
    // check if error model for filename already exists
    pErrorStruct errormodel = get_error_model(family, filename);

    if (errormodel == NULL)
    {
        // allocate new errormodel
        errormodel = (pErrorStruct)calloc(1, sizeof(ErrorStruct));
        errormodel->errorfilename = strdup(filename.c_str());
        errormodel->maxfamilysize = range.max;
        std::ifstream ifst(filename.c_str());
        if (!ifst)
            throw io_error("errormodel", filename, false);

        ifst >> *errormodel;

        // now make sure that columns of the error matrix sums to one.
        __check_error_model_columnsums(errormodel);

        if (family->errors == NULL) {
            family->errors = arraylist_new(family->num_species);
        }
        arraylist_add(family->errors, errormodel);

    }
    init_error_ptr(family, pTree, errormodel, speciesname);
    return 0;
}

int remove_error_model(pCafeFamily family, pCafeTree pcafe, std::string species_name)
{
    int i = 0;
    if (family->errors) {
        assert(family->error_ptr != NULL);      // errors and error_ptr move in lockstep

        for (i = 0; i<family->num_species; i++) {
            if (case_insensitive_equal(family->species[i], species_name)) {
                family->error_ptr[i] = NULL;
                pCafeNode pcnode = (pCafeNode)pcafe->super.nlist->array[family->index[i]];
                pcnode->errormodel = NULL;
                break;
            }
        }
    }
    return 0;
}

void free_error_model(pCafeFamily family, pCafeTree pcafe)
{
    int i;
    for (i = 0; i<family->num_species; i++) {
        remove_error_model(family, pcafe, family->species[i]);
    }
    if (family->errors) {
        arraylist_free(family->errors, (freefunc)cafe_shell_free_errorstruct);
        family->errors = NULL;
    }
    if (family->error_ptr) {
        memory_free(family->error_ptr);
        family->error_ptr = NULL;
    }
}

void read_freq_from_measures(std::istream* ist1, std::istream* ist2, int* sizeFreq, int& maxFamilySize)
{
    if (!*ist1)
    {
        throw io_error("errest", "measure 1", false);
    }
    std::string str;
    if (!std::getline(*ist1, str))
    {
        throw io_error("errest", "measure 1", false);
    }
    if (ist2 != NULL) {     // if there is a file 2
        if (!*ist2)     // if file 2 is invalid
        {
            throw io_error("errest", "measure 2", false);
        }
        if (!std::getline(*ist2, str))
        {
            throw io_error("errest", "measure 2", false);
        }
    }

    // count frequency of family sizes
    int line1 = 0;
    // int maxFamilySize = cafe_param->family_size.max;
    size_t data1colnum = 0;
    while (std::getline(*ist1, str))
    {
        std::istringstream ist(str);
        gene_family gf;
        ist >> gf;

        for (size_t i = 0; i < gf.values.size(); i++) {
            sizeFreq[gf.values[i]]++;
        }

        maxFamilySize = std::max(maxFamilySize, *std::max_element(gf.values.begin(), gf.values.end()));

        data1colnum = gf.values.size();
        line1++;
    }
    if (ist2 != NULL)
    {
        int line2 = 0;
        while (std::getline(*ist2, str))
        {
            std::istringstream ist(str);
            gene_family gf;
            ist >> gf;

            if (data1colnum != gf.values.size()) {
                throw std::runtime_error("file: the number of columns do not match between the two files\n");
            }
            for (size_t i = 0; i < gf.values.size(); i++) {
                sizeFreq[gf.values[i]]++;
            }

            maxFamilySize = std::max(maxFamilySize, *std::max_element(gf.values.begin(), gf.values.end()));

            line2++;
        }
        if (line1 != line2) {
            throw std::runtime_error("The number of lines do not match between the two files");
        }
    }
}




void read_error_double_measure(std::istream& ist1, std::istream& ist2, int** observed_pairs, int maxFamilySize)
{
    std::string buf1;
    std::string buf2;

    if (!ist1)
    {
        throw io_error("errest", "measure 1", false);
    }
    if (!std::getline(ist1, buf1))
    {
        throw io_error("errest", "measure 1", false);
    }
    if (!ist2)
    {
        throw io_error("errest", "measure 2", false);
    }
    if (!std::getline(ist2, buf2))
    {
        throw io_error("errest", "measure 2", false);
    }


    // now compare two files and count pairs.
    while (std::getline(ist1, buf1))
    {
        if (std::getline(ist2, buf2)) {
            std::istringstream sst1(buf1), sst2(buf2);
            gene_family gf1, gf2;
            sst1 >> gf1;
            sst2 >> gf2;

            if (gf1.id != gf2.id) {
                throw std::runtime_error("ERROR: the family IDs in each line do not match between the two files\n");
            }
            // check pairs
            for (size_t i = 0; i<gf1.values.size(); i++) {
                observed_pairs[gf1.values[i]][gf2.values[i]]++;
            }
        }
    }

    // now make triangle matrix by merging i,j and j,i
    // TODO: Doesn't seem to work as intended
    for (int i = 0; i <= maxFamilySize; i++) {
        for (int j = 0; j<i; j++) {
            observed_pairs[j][i] += observed_pairs[i][j];
            observed_pairs[i][j] = 0;
        }
    }
}

double get_marginal_error_probability_epsilon(pErrorMeasure errormeasure, double *parameters)
{
    int i;
    double marginal_error_probability_epsilon = -1;
    if (errormeasure->b_symmetric) {
        // symmetric
        double sum = parameters[0];
        for (i = 1; i<errormeasure->model_parameter_number; i++) {
            sum += 2 * parameters[i];
        }
        marginal_error_probability_epsilon = (1 - sum) / (double)((errormeasure->maxFamilySize + 1) - (errormeasure->model_parameter_diff * 2 + 1));
    }
    else {
        //asymmetric
        double sum = 0;
        for (i = 0; i<errormeasure->model_parameter_number; i++) {
            sum += parameters[i];
        }
        marginal_error_probability_epsilon = (1 - sum) / (double)((errormeasure->maxFamilySize + 1) - (errormeasure->model_parameter_diff * 2 + 1));
    }

    return marginal_error_probability_epsilon;
}

double __loglikelihood_pairs_from_double_measure(double* parameters, void* args)
{
    int i, j, k;

    std::pair<pErrorMeasure, std::ostream&> *x = (std::pair<pErrorMeasure, std::ostream&>*)args;

    pErrorMeasure errormeasure = x->first;
    std::ostream& log_stream = x->second;

    double marginal_error_probability_epsilon = get_marginal_error_probability_epsilon(errormeasure, parameters);

    double score = 0;
    int skip = 0;
    for (i = 0; i < errormeasure->model_parameter_number; i++)
    {
        if ((parameters[i] < 0) || (marginal_error_probability_epsilon < 0) || (marginal_error_probability_epsilon > parameters[i]))
        {
            skip = 1;
            score = log(0);
            break;
        }
    }
    if (!skip && errormeasure->b_peakzero) {
        double previous_param = 0;
        if (errormeasure->b_symmetric) {
            previous_param = parameters[0];
            for (i = 1; i<errormeasure->model_parameter_number; i++) {
                if (previous_param < parameters[i]) {
                    skip = 1;
                    score = log(0);
                    break;
                }
                previous_param = parameters[i];
            }
        }
        else {
            previous_param = parameters[errormeasure->model_parameter_diff];
            for (i = 1; i <= errormeasure->model_parameter_diff; i++) {
                if (previous_param < parameters[errormeasure->model_parameter_diff - i]) {
                    skip = 1;
                    score = log(0);
                    break;
                }
                previous_param = parameters[errormeasure->model_parameter_diff - i];
            }
            previous_param = parameters[errormeasure->model_parameter_diff];
            for (i = 1; i <= errormeasure->model_parameter_diff; i++) {
                if (previous_param < parameters[errormeasure->model_parameter_diff + i]) {
                    skip = 1;
                    score = log(0);
                    break;
                }
                previous_param = parameters[errormeasure->model_parameter_diff + i];
            }
        }
    }
    if (!skip)
    {
        errormeasure->estimates = parameters;
        pErrorStruct errormodel = cafe_shell_create_error_matrix_from_estimate(errormeasure);

        double** discord_prob_model = (double**)memory_new_2dim(errormeasure->maxFamilySize + 1, errormeasure->maxFamilySize + 1, sizeof(double));
        for (i = 0; i <= errormeasure->maxFamilySize; i++) {
            for (j = i; j <= errormeasure->maxFamilySize; j++) {
                for (k = 0; k <= errormeasure->maxFamilySize; k++) {
                    double pi_i_k = errormodel->errormatrix[i][k];
                    double pi_j_k = errormodel->errormatrix[j][k];
                    if (i == j) {
                        discord_prob_model[i][j] += errormeasure->sizeDist[k] * pi_i_k*pi_j_k;
                    }
                    else {
                        discord_prob_model[i][j] += 2 * errormeasure->sizeDist[k] * pi_i_k*pi_j_k;
                    }
                }
            }
        }
        for (i = 0; i <= errormeasure->maxFamilySize; i++) {
            for (j = i; j <= errormeasure->maxFamilySize; j++) {
                // add to the log likelihood
                double term = errormeasure->pairs[i][j] ? errormeasure->pairs[i][j] * log(discord_prob_model[i][j]) : 0;
                score += term;
                if (std::isnan(score) || std::isinf(-score) || !std::isfinite(score)) {
                    log_stream << "Score: " << score << std::endl;
                    break;
                }
            }
        }
        double prob00 = 0;
        for (k = 0; k <= errormeasure->maxFamilySize; k++) {
            double pi_i_k = errormodel->errormatrix[0][k];
            double pi_j_k = errormodel->errormatrix[0][k];
            prob00 += errormeasure->sizeDist[k] * pi_i_k*pi_j_k;
        }
        score -= log(1 - prob00);

        memory_free_2dim((void**)discord_prob_model, errormeasure->maxFamilySize + 1, errormeasure->maxFamilySize + 1, NULL);
        cafe_shell_free_errorstruct(errormodel);

    }

    char buf[STRING_STEP_SIZE];
    buf[0] = '\0';
    string_pchar_join_double(buf, ",", errormeasure->model_parameter_number, parameters);
    log_stream << "\tparameters : " << buf << " & Score: " << score << std::endl;
    return -score;
}

void get_size_probability_distribution(int maxFamilySize, int *sizeFreq, double* sizeDist)
{
    // get size probability distribution
    int sizeTotal = 0;
    for (int i = 0; i <= maxFamilySize; i++) {
        sizeTotal += sizeFreq[i] + 1;
        if (sizeTotal < 0) {
            fprintf(stderr, "ERROR: total freqeuncy is less than zero\n");
        }
    }
    for (int i = 0; i <= maxFamilySize; i++) {
        sizeDist[i] = (sizeFreq[i] + 1) / (double)sizeTotal;
        if (sizeDist[i] < 0) {
            fprintf(stderr, "ERROR: freqeuncy is less than zero\n");
        }
    }


}

pErrorMeasure estimate_error_double_measure(std::ostream& log, std::istream* ist1, std::istream* ist2, int b_symmetric, int max_diff, int b_peakzero, int max_FamilySize)
{
    int i;
    int* sizeFreq = (int *)memory_new(10000, sizeof(int));
    int maxFamilySize = max_FamilySize;
    read_freq_from_measures(ist1, ist2, sizeFreq, maxFamilySize);

    double* sizeDist = (double*)memory_new(maxFamilySize + 1, sizeof(double));

    get_size_probability_distribution(maxFamilySize, sizeFreq, sizeDist);
    int** observed_pairs = (int**)memory_new_2dim(maxFamilySize + 1, maxFamilySize + 1, sizeof(int)); // need space for zero
    ist1->clear();
    ist1->seekg(0, std::ios::beg);

    ist2->clear();
    ist2->seekg(0, std::ios::beg);
    read_error_double_measure(*ist1, *ist2, observed_pairs, maxFamilySize);

    // set up parameters for ML
    pErrorMeasure error = (pErrorMeasure)memory_new(1, sizeof(ErrorMeasure));
    error->sizeDist = sizeDist;
    error->maxFamilySize = maxFamilySize;
    error->pairs = observed_pairs;
    error->b_symmetric = b_symmetric;
    error->b_peakzero = b_peakzero;
    if (b_symmetric) {
        // symmetric model (diff == number)
        error->model_parameter_diff = max_diff;
        error->model_parameter_number = max_diff + 1;
    }
    else {
        // asymmetric model (diff*2 == number)
        error->model_parameter_diff = max_diff;
        error->model_parameter_number = 2 * max_diff + 1;
    }


    // now estimate the misclassification rate
    int max_runs = 100;
    int converged = 0;
    int runs = 0;
    double minscore = DBL_MAX;
    double* parameters = (double *)memory_new(error->model_parameter_number, sizeof(double));
    double* bestrun_parameters = (double *)memory_new(error->model_parameter_number, sizeof(double));

    do {
        pFMinSearch pfm;
        double* sorted_params = (double *)memory_new_with_init(error->model_parameter_number, sizeof(double), parameters);
        for (i = 0; i<error->model_parameter_number; i++) {
            sorted_params[i] = unifrnd() / (double)error->model_parameter_number;
        }
        qsort(sorted_params, error->model_parameter_number, sizeof(double), comp_double);
        if (error->b_symmetric) {
            int j = 0;
            for (i = error->model_parameter_number - 1; i >= 0; i--) {
                parameters[j++] = sorted_params[i];
            }
        }
        else {
            int j = error->model_parameter_number - 1;
            parameters[error->model_parameter_diff] = sorted_params[j--];
            for (i = 1; i <= error->model_parameter_diff; i++) {
                parameters[error->model_parameter_diff - i] = sorted_params[j--];
                parameters[error->model_parameter_diff + i] = sorted_params[j--];
            }
        }
        std::pair<pErrorMeasure, std::ostream&> args(error, log);
        pfm = fminsearch_new_with_eq(__loglikelihood_pairs_from_double_measure, error->model_parameter_number, &args);
        pfm->tolx = 1e-9;
        pfm->tolf = 1e-9;
        fminsearch_min(pfm, parameters);
        double *re = fminsearch_get_minX(pfm);
        for (i = 0; i < error->model_parameter_number; i++) parameters[i] = re[i];
        log << std::endl << "Misclassification Matrix Search Result: (" << pfm->iters << " iterations)\n";
        log << "Score: " << *pfm->fv << std::endl;

        if (runs > 0) {
            if (!std::isnan(*pfm->fv) && !std::isinf(*pfm->fv) && abs(minscore - (*pfm->fv)) < pfm->tolf) {
                converged = 1;
            }
        }
        if (pfm->iters < pfm->maxiters) {
            if (*pfm->fv < minscore) {
                minscore = *pfm->fv;
                memcpy(bestrun_parameters, parameters, (error->model_parameter_number) * sizeof(double));
            }
            runs++;
        }
        /*        else {
        cafe_log(param,"what went wrong?\n");
        fminsearch_min(pfm, parameters);
        }*/
        fminsearch_free(pfm);
    } while (!converged && runs<max_runs);

    if (converged) {
        log << "score converged in " << runs << " runs." << std::endl;
    }
    else {
        log << "score failed to converge in " << max_runs << " runs." << std::endl;
        log << "best score: " << minscore << std::endl;
    }
    memory_free(parameters);
    error->estimates = bestrun_parameters;

    //memory_free(error);           // we are going to return these values
    memory_free_2dim((void**)observed_pairs, maxFamilySize + 1, maxFamilySize + 1, NULL);
    memory_free(sizeFreq);
    return error;
}

pErrorMeasure estimate_error_true_measure(std::ostream& log, const char* errorfile, const char* truefile, int b_symmetric, int max_diff, int b_peakzero, int max_family_size)
{
    int i;

    int* sizeFreq = (int *)memory_new(10000, sizeof(int));
    std::ifstream err(errorfile), true_(truefile);
    int maxFamilySize = max_family_size;
    read_freq_from_measures(&true_, errorfile == NULL ? NULL : &err, sizeFreq, maxFamilySize);

    // get size probability distribution
    int sizeTotal = 0;
    for (i = 0; i <= maxFamilySize; i++) {
        sizeTotal += sizeFreq[i] + 1;
    }
    double* sizeDist = (double*)memory_new(maxFamilySize + 1, sizeof(double));
    for (i = 0; i <= maxFamilySize; i++) {
        sizeDist[i] = (sizeFreq[i] + 1) / (double)sizeTotal;
    }


    int** observed_pairs = (int**)memory_new_2dim(maxFamilySize + 1, maxFamilySize + 1, sizeof(int)); // need space for zero
    int retval = cafe_shell_read_error_true_measure(errorfile, truefile, observed_pairs, maxFamilySize);
    if (retval < 0) {
        fprintf(stderr, "ERROR: failed to count pairs from measurement files\n");
    }

    // set up parameters for ML
    pErrorMeasure error = (pErrorMeasure)memory_new(1, sizeof(ErrorMeasure));
    error->sizeDist = sizeDist;
    error->maxFamilySize = maxFamilySize;
    error->pairs = observed_pairs;
    error->b_symmetric = b_symmetric;
    error->b_peakzero = b_peakzero;
    if (b_symmetric) {
        // symmetric model (diff == number)
        error->model_parameter_diff = max_diff;
        error->model_parameter_number = max_diff + 1;
    }
    else {
        // asymmetric model (diff*2 == number)
        error->model_parameter_diff = max_diff;
        error->model_parameter_number = 2 * max_diff + 1;
    }

    // now estimate the misclassification rate
    int max_runs = 100;
    int converged = 0;
    int runs = 0;
    double minscore = DBL_MAX;
    double* parameters = (double *)memory_new(error->model_parameter_number, sizeof(double));
    double* bestrun_parameters = (double *)memory_new(error->model_parameter_number, sizeof(double));

    do {
        pFMinSearch pfm;
        double* sorted_params = (double *)memory_new_with_init(error->model_parameter_number, sizeof(double), parameters);
        for (i = 0; i<error->model_parameter_number; i++) {
            sorted_params[i] = unifrnd() / (double)error->model_parameter_number;
        }
        qsort(sorted_params, error->model_parameter_number, sizeof(double), comp_double);
        if (error->b_symmetric) {
            int j = 0;
            for (i = error->model_parameter_number - 1; i >= 0; i--) {
                parameters[j++] = sorted_params[i];
            }
        }
        else {
            int j = error->model_parameter_number - 1;
            parameters[error->model_parameter_diff] = sorted_params[j--];
            for (i = 1; i <= error->model_parameter_diff; i++) {
                parameters[error->model_parameter_diff - i] = sorted_params[j--];
                parameters[error->model_parameter_diff + i] = sorted_params[j--];
            }
        }

        pfm = fminsearch_new_with_eq(__loglikelihood_pairs_from_true_measure, error->model_parameter_number, error);
        pfm->tolx = 1e-9;
        pfm->tolf = 1e-9;
        fminsearch_min(pfm, parameters);
        double *re = fminsearch_get_minX(pfm);
        for (i = 0; i < error->model_parameter_number; i++) parameters[i] = re[i];
        log << std::endl << "Misclassification Matrix Search Result: (" << pfm->iters << " iterations)\n";
        log << "Score: " << *pfm->fv << std::endl;

        if (runs > 0) {
            if (!std::isnan(*pfm->fv) && !std::isinf(*pfm->fv) && abs(minscore - (*pfm->fv)) < pfm->tolf) {
                converged = 1;
            }
        }
        if (pfm->iters < pfm->maxiters) {
            if (*pfm->fv < minscore) {
                minscore = *pfm->fv;
                memcpy(bestrun_parameters, parameters, (error->model_parameter_number) * sizeof(double));
            }
            runs++;
        }
        fminsearch_free(pfm);
    } while (!converged && runs<max_runs);

    if (converged) {
        log << "score converged in " << runs << " runs." << std::endl;
    }
    else {
        log << "score failed to converge in " << max_runs << " runs." << std::endl;
        log << "best score: " << minscore << std::endl;
    }

    memory_free(parameters);
    error->estimates = bestrun_parameters;

    //memory_free(error);           // we are going to return these values
    memory_free_2dim((void**)observed_pairs, maxFamilySize + 1, maxFamilySize + 1, NULL);
    memory_free(sizeFreq);
    return error;
}