#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/error/en.h"

#include <immintrin.h>

#include <iostream>

#include "common.h"

using namespace rapidjson;

#define VECSIZE 32

double fast() {
    char *raw;
    size_t length = read_all(path_for_data("amazon_video_reviews.json"), &raw);

    int doc_index = 1;
    double score_average = 0.0;

    bench_timer_t s = time_start();

    char numbuf[1024];

    // Current line.
    char *line = raw;
    // Finds newline characters.
    __m256i line_seeker = _mm256_set1_epi8('\n');
    for (long i = 0; i < length; i += VECSIZE) {
        __m256i word = _mm256_load_si256((__m256i const *)(raw + i));
        // Last iteration - mask out bytes past the end of the input
        if (i + VECSIZE > length) {
            // mask out unused "out of bounds" bytes.
            // This is slow...optimize.
            __m256i eraser = _mm256_cmpeq_epi8(line_seeker, line_seeker);
            for (int j = 0; j < i + VECSIZE - length; j++) {
                eraser = _mm256_insert_epi8(eraser, 0, VECSIZE - j - 1);
            }
            word = _mm256_and_si256(word, eraser);
        }

        __m256i mask = _mm256_cmpeq_epi8(word, line_seeker);
        int imask = _mm256_movemask_epi8(mask);
        while (imask) {
            int idx = ffs(imask) - 1;
            raw[idx + i] = '\0';
            //printf("line %ld -> %d: %s\n", line - raw, idx + i, line);

            // Process `line` here. Length of the line is (idx + i) - (line - raw).
            
            /////////////
            // Begin Token Processing. 
            int line_length = (idx + i) - (line - raw);

            // TODO - nesting - we don't need to parse the whole tree hopefully, but just 
            // check if the nesting level of this particular "overall" is correct.
            // TODO - what if we need to parse out multiple fields? Do we check this token for all
            // the fields? That would be an O(mn^2) operation where m is the number of fields to
            // parse and n is the length of the string.
            char *field_offset = strnstr(line, "\"overall\":", line_length);
            // Not general enough right now, but whatever
            field_offset += strlen("\"overall\":");
            char *endptr;
            float f = strtof(field_offset, &endptr);
            if (endptr == field_offset) {
                printf("%s\n", field_offset);
            }
            score_average += f;
            doc_index++;

            // End Token Processing. 
            ////////////

end_line_processing:
            line = raw + i + idx + 1;
            imask &= ~(1 << idx);
        }

        // TODO Special processing for the last line?
    }

    double elapsed = time_stop(s);
    printf("Custom Average overall score: %f (%.3f seconds)\n", score_average / doc_index, elapsed);
    return elapsed;
}

double rapidjson_fast_newline() {
    char *raw;
    size_t length = read_all(path_for_data("tweets.json"), &raw);

    long count = 0;

    int doc_index = 1;
    double score_average = 0.0;

    Value::ConstMemberIterator itr;

    bench_timer_t s = time_start();

    // Current line.
    char *line = raw;
    // Finds newline characters.
    __m256i line_seeker = _mm256_set1_epi8('\n');
    for (long i = 0; i < length; i += VECSIZE) {
        __m256i word = _mm256_load_si256((__m256i const *)(raw + i));
        // Last iteration - mask out bytes past the end of the input
        if (i + VECSIZE > length) {
            // mask out unused "out of bounds" bytes.
            // This is slow...optimize.
            __m256i eraser = _mm256_cmpeq_epi8(line_seeker, line_seeker);
            for (int j = 0; j < i + VECSIZE - length; j++) {
                eraser = _mm256_insert_epi8(eraser, 0, VECSIZE - j - 1);
            }
            word = _mm256_and_si256(word, eraser);
        }

        __m256i mask = _mm256_cmpeq_epi8(word, line_seeker);
        int imask = _mm256_movemask_epi8(mask);
        while (imask) {
            int idx = ffs(imask) - 1;
            raw[idx + i] = '\0';
            //printf("line %ld -> %d: %s\n", line - raw, idx + i, line);

            // Process `line` here. Length of the line is (idx + i) - (line - raw).
            
            /////////////
            // Begin Token Processing. 
            int line_length = (idx + i) - (line - raw);

            Document d;
            d.Parse(line);
            if (d.HasParseError()) {
                fprintf(stderr, "\nError(offset %u): %s\n", 
                        (unsigned)d.GetErrorOffset(),
                        GetParseError_En(d.GetParseError()));
                goto end_line_processing;
            }


#if 0
static const char* kTypeNames[] = 
    { "Null", "False", "True", "Object", "Array", "String", "Number" };
for (Value::ConstMemberIterator itr = d.MemberBegin();
    itr != d.MemberEnd(); ++itr)
{
    printf("Type of member %s is %s\n",
        itr->name.GetString(), kTypeNames[itr->value.GetType()]);
}
#endif

            if (d.HasMember("favorited")) {
                count++;
            }

            doc_index++;
            
            // End Token Processing. 
            ////////////

end_line_processing:
            line = raw + i + idx + 1;
            imask &= ~(1 << idx);
        }

        // TODO Special processing for the last line?
    }

    double elapsed = time_stop(s);
    printf("Rapid Count: %ld (%f%%) (%.3f seconds)\n", count, (float)count / (float)doc_index, elapsed);
    return elapsed;
}

double baseline_rapidjson() {
    char *data, *line;
    size_t bytes = read_all(path_for_data("tweets.json"), &data);
    int doc_index = 1;

    double score_average = 0.0;

    bench_timer_t s = time_start();

    while ((line = strsep(&data, "\n")) != NULL) {
        Document d;
        d.Parse(line);
        if (d.HasParseError()) {
            fprintf(stderr, "\nError(offset %u): %s\n", 
                    (unsigned)d.GetErrorOffset(),
                    GetParseError_En(d.GetParseError()));
            continue;
        }

        doc_index++;
    }

    double elapsed = time_stop(s);
    printf("Baseline Average overall score: %f (%.3f seconds)\n", score_average / doc_index, elapsed);
    return elapsed;
}

int main() {
    double a = baseline_rapidjson();
    double b = rapidjson_fast_newline();
    //double c = fast();
    //printf("Speedup: %.3f\n", a / c);
}