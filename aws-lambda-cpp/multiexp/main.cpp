// main.cpp
#include <algorithm>
#include <cassert>
#include <type_traits>
#include <sstream>
#include <iterator>
#include <iostream>
#include <aws/core/Aws.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/logging/LogLevel.h>
#include <aws/core/utils/logging/ConsoleLogSystem.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/HashingUtils.h>
#include <libff/common/utils.hpp>
#include <libff/common/serialization.hpp>
#include <libff/algebra/fields/bigint.hpp>
#include <libff/algebra/curves/bn128/bn128_pp.hpp>
#include <libff/algebra/fields/fp.hpp>
#include <libff/algebra/fields/field_utils.hpp>
#include <libff/algebra/fields/fp.tcc>
#include <libff/algebra/fields/bigint.hpp>
#include <libff/algebra/fields/fp_aux.tcc>
#include <libff/algebra/scalar_multiplication/multiexp.hpp>
#include <libff/algebra/scalar_multiplication/wnaf.hpp>
//#include <libff/common/profiling.hpp>
#include <libff/common/utils.hpp>
#include <libff/common/rng.hpp>
#include <aws/lambda-runtime/runtime.h>

using namespace libff;
using namespace aws::lambda_runtime;
using namespace Aws::Utils;

namespace libff {
template<typename T, typename FieldT, multi_exp_method Method,
    typename std::enable_if<(Method == multi_exp_method_BDLO12), int>::type = 0>
T multi_exp_inner1(
    typename std::vector<T>::const_iterator bases,
    typename std::vector<T>::const_iterator bases_end,
    typename std::vector<FieldT>::const_iterator exponents,
    typename std::vector<FieldT>::const_iterator exponents_end)
    {
    	UNUSED(exponents_end);
    	size_t length = bases_end - bases;

    	// empirically, this seems to be a decent estimate of the optimal value of c
    	size_t log2_length = log2(length);
    	size_t c = log2_length - (log2_length / 3 - 2);

    	const mp_size_t exp_num_limbs =
        	std::remove_reference<decltype(*exponents)>::type::num_limbs;
    	std::vector<bigint<exp_num_limbs> > bn_exponents(length);
    	size_t num_bits = 0;

    	for (size_t i = 0; i < length; i++)
    	{
        	bn_exponents[i] = exponents[i].as_bigint();
        	num_bits = std::max(num_bits, bn_exponents[i].num_bits());
    	}

    	size_t num_groups = (num_bits + c - 1) / c;

    	T result;
    	bool result_nonzero = false;

    	for (size_t k = num_groups - 1; k <= num_groups; k--)
    	{
        	if (result_nonzero)
        	{
            		for (size_t i = 0; i < c; i++)
            		{
                		result = result.dbl();
            		}
        	}

        	std::vector<T> buckets(1 << c);
        	std::vector<bool> bucket_nonzero(1 << c);

        	for (size_t i = 0; i < length; i++)
        	{
            		size_t id = 0;
            		for (size_t j = 0; j < c; j++)
           	 	{
                		if (bn_exponents[i].test_bit(k*c + j))
                		{
                    			id |= 1 << j;
                		}
            		}

            		if (id == 0)
            		{
                		continue;
            		}

            		if (bucket_nonzero[id])
            		{
#ifdef USE_MIXED_ADDITION
                		buckets[id] = buckets[id].mixed_add(bases[i]);
#else
                		buckets[id] = buckets[id] + bases[i];
#endif
            		}
           		else
            		{
                		buckets[id] = bases[i];
                		bucket_nonzero[id] = true;
            		}
        	}

#ifdef USE_MIXED_ADDITION
        	batch_to_special(buckets);
#endif

        	T running_sum;
        	bool running_sum_nonzero = false;

        	for (size_t i = (1u << c) - 1; i > 0; i--)
        	{
            		if (bucket_nonzero[i])
            		{
                		if (running_sum_nonzero)
                		{
#ifdef USE_MIXED_ADDITION
                    			running_sum = running_sum.mixed_add(buckets[i]);
#else
                    			running_sum = running_sum + buckets[i];
#endif
                		}
                		else
                		{
                    			running_sum = buckets[i];
                    			running_sum_nonzero = true;
                		}	
            		}

            		if (running_sum_nonzero)
            		{
                		if (result_nonzero)
                		{
                    			result = result + running_sum;
                		}
                		else
                		{
                    			result = running_sum;
                    			result_nonzero = true;
                		}
            		}
        	}
    	}

	return result;
    	//return invocation_response::success(result, "application/json");
    }


template <typename GroupT>
using run_result_t = std::pair<long long, std::vector<GroupT> >;

template <typename T>
using test_instances_t = std::vector<std::vector<T> >;

template<typename GroupT>
test_instances_t<GroupT> generate_group_elements(size_t count, size_t size)
{
    // generating a random group element is expensive,
    // so for now we only generate a single one and repeat it
    test_instances_t<GroupT> result(count);

    for (size_t i = 0; i < count; i++) {
        GroupT x = GroupT::random_element();
        x.to_special(); // djb requires input to be in special form
        for (size_t j = 0; j < size; j++) {
            result[i].push_back(x);
            // result[i].push_back(GroupT::random_element());
        }
    }

    return result;
}

template<typename FieldT>
test_instances_t<FieldT> generate_scalars(size_t count, size_t size)
{
    // we use SHA512_rng because it is much faster than
    // FieldT::random_element()
    test_instances_t<FieldT> result(count);

    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < size; j++) {
            result[i].push_back(SHA512_rng<FieldT>(i * size + j));
        }
    }

    return result;
}
}

int multi_exp_run()
{
   libff::bn128_pp::init_public_params();
   size_t expn = 2;

   test_instances_t<G1<libff::bn128_pp>> group_elements =
            libff::generate_group_elements<G1<libff::bn128_pp>>(10, 1 << expn);
   test_instances_t<Fr<libff::bn128_pp>> scalars =
            libff::generate_scalars<Fr<libff::bn128_pp>>(10, 1 << expn);

   std::vector<G1<bn128_pp>> answers;
   for (size_t i = 0; i < group_elements.size(); i++) {
        std::ostringstream oss;

	for(size_t j=0; j<group_elements[i].size(); j++) {
		printf("%d %lld ",j, group_elements[i].at(j));fflush(stdout);
	}
	printf("\n");
   	if (!group_elements[i].empty())
   	{
		printf("before serializing: \t%lld\n", group_elements[i].at(2));
     		// Convert all but the last element to avoid a trailing ","
     		std::copy(group_elements[i].begin(), group_elements[i].end()-1,
        	std::ostream_iterator<G1<bn128_pp>>(oss, " "));

     		// Now add the last element with no delimiter
     		oss << group_elements[i].back();
	}

   	//std::cout << oss.str() << std::endl;
	std::stringstream iss( oss.str() );
	printf("Serialized: %s\n", oss.str());

	std::stringstream is( oss.str() );
	std::vector<G1<bn128_pp>> myNumbers{ std::istream_iterator<G1<bn128_pp>>( is ), std::istream_iterator<G1<bn128_pp>>() };
	
	//std::istream_iterator<G1<bn128_pp>>
	printf("after serializing: \t%lld\n", myNumbers.at(3));
    
        answers.push_back(libff::multi_exp_inner1<G1<bn128_pp>, Fr<bn128_pp>, multi_exp_method_BDLO12,
      1>(group_elements[i].cbegin(), group_elements[i].cend(), scalars[i].cbegin(), scalars[i].cend()));
   } 
   
   return answers.size();
}

// Deserialize a string into a vector
//
template<typename T>
std::vector<T> deserializeToVec(std::string s_vec) {
	std::istringstream is(s_vec.c_str());
        return std::vector<T>{ std::istream_iterator<T>( is ), 
                        std::istream_iterator<T>() };
}

// Deserialize a string into a single element
//
template<typename T>
T deserialize(std::string s_elem) {
	T ret;
	std::istringstream is(s_elem.c_str());
    is >> ret;
	return ret;
}

// Serialize a vector into a string
//
template<typename T> 
std::string serializeVec(std::vector<T> vec) {
	std::ostringstream oss;
	std::string str = "";
	std::copy(vec.begin(), vec.end()-1,
                std::ostream_iterator<T>(oss, " "));

        // Now add the last element with no delimiter
        oss << vec.back();
	return oss.str();
}

// Serialize a single element into a string
//
template<typename T> 
std::string serialize(T elem) {
	std::ostringstream oss;
	std::string str = "";
    oss << elem;
	return oss.str();
}

template<typename T>
std::string serializeFieldVec(std::vector<T> vec) {
    std::ostringstream oss;
    libff::bit_vector bv = libff::convert_field_element_vector_to_bit_vector<T>(vec);
	libff::serialize_bit_vector(oss, bv);
    return oss.str();
}

template<typename T>
std::string serializeField(T field) {
    std::ostringstream oss;
    libff::bit_vector bv = libff::convert_field_element_to_bit_vector<T>(field);
	libff::serialize_bit_vector(oss, bv);
    return oss.str();
}

template<typename T>
std::vector<T> deserializeFieldVec(std::string s_vec) {
    std::istringstream is(s_vec.c_str());
    bit_vector bv;
    libff::deserialize_bit_vector(is, bv);
    return libff::convert_bit_vector_to_field_element_vector<T>(bv);
}

template<typename T>
T deserializeField(std::string s_field) {
    std::istringstream is(s_field.c_str());
    bit_vector bv;
    libff::deserialize_bit_vector(is, bv);
    return libff::convert_bit_vector_to_field_element<T>((const bit_vector)bv);
}

std::string invoke_multiexp_inner(
	std::vector<G1<bn128_pp>> groupElement,
	std::vector<Fr<bn128_pp>> scalar)
{
	G1<bn128_pp> answer = 
	multi_exp_inner1<G1<bn128_pp>,
					 Fr<bn128_pp>,
					 multi_exp_method_BDLO12,
					 1>
					 (groupElement.cbegin(),
					 groupElement.cend(),
					 scalar.cbegin(),
					 scalar.cend());
	return serialize<G1<bn128_pp>>(answer);
}



int test_deserialize(std::string key) {
	libff::bn128_pp::init_public_params();

	std::istringstream is(key.c_str());
        std::vector<G1<bn128_pp>> myNumbers{ std::istream_iterator<G1<bn128_pp>>( is ), 
			std::istream_iterator<G1<bn128_pp>>() };

	return 3;
}


/*
invocation_response my_handler(invocation_request const& request)
{
   using namespace Aws::Utils::Json;
    JsonValue json((const Aws::String&)request.payload);
    if (!json.WasParseSuccessful()) {
        return invocation_response::failure("1Failed to parse input JSON", "InvalidJSON");
    }

    auto v = json.View();

    if (!v.ValueExists("groupelements")) {
        return invocation_response::failure("2Failed to parse input JSON", "InvalidJSON");
    }

    auto key1 = v.GetString("groupelements");

   Aws::String d_str = Aws::Utils::StringUtils::URLDecode(key1.c_str()); 
   int len = test_deserialize(d_str.c_str());

   return invocation_response::success("hello", "application/json");
}
*/

invocation_response multiexp_inner_handler(invocation_request const& request)
{
   using namespace Aws::Utils::Json;
    JsonValue json((const Aws::String&)request.payload);
    if (!json.WasParseSuccessful()) {
        return invocation_response::failure("1Failed to parse input JSON", "InvalidJSON");
    }

    auto v = json.View();

    if (!v.ValueExists("groupelements")) {
        return invocation_response::failure("2Failed to parse input JSON", "InvalidJSON");
    }

    auto groupElements_str = v.GetString("groupelements");

	if (!v.ValueExists("scalars")) {
        return invocation_response::failure("3Failed to parse input JSON", "InvalidJSON");
    }

    auto scalars_str = v.GetString("scalars");

    Aws::String ge_d_str = 
		Aws::Utils::StringUtils::URLDecode(groupElements_str.c_str()); 
	Aws::String sc_d_str = 
		Aws::Utils::StringUtils::URLDecode(scalars_str.c_str()); 
   
    std::vector<G1<bn128_pp>> groupelements = 
		deserializeToVec<G1<bn128_pp>>(ge_d_str.c_str());
	std::vector<Fr<bn128_pp>> scalars = 
		deserializeToVec<Fr<bn128_pp>>(sc_d_str.c_str());
	std::string answer = 
		invoke_multiexp_inner(groupelements, scalars);
	//deserialize<Fr<bn128_pp>>("56");
	//serialize<Fr<bn128_pp>>(scalars.at(0));
	//std::ostringstream oss;
	//std::string str = "";
	//libff::serialize_bit_vector(oss, libff::convert_field_element_to_bit_vector(scalars.at(0)));
    //std::cout << libff::bn128_Fr(scalars.at(0));
    return invocation_response::success(answer.c_str(), "application/json");
}

int main()
{
   run_handler(multiexp_inner_handler);
   //multi_exp_run();
    
   return 0;
}


