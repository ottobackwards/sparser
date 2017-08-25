#ifndef _SPARSER_H_
#define _SPARSER_H_

#include <immintrin.h>

#include <stdio.h>
#include <string.h>

// Checks if bit i is set in n.
#define SET(n, i) (n & (0x1 << i))

// Size of the register we use.
const int VECSZ = 32;
// Max size of a single search string.
const int SPARSER_MAX_QUERY_LENGTH = 4 + 1;
// Max number of search strings in a single query.
const int SPARSER_MAX_QUERY_COUNT = 2;

// Defines a sparser query.
typedef struct sparser_query_ {
  unsigned count;
  char queries[SPARSER_MAX_QUERY_COUNT][SPARSER_MAX_QUERY_LENGTH];
  size_t lens[SPARSER_MAX_QUERY_COUNT];
} sparser_query_t;

/// Takes a register containing a search token and a base address, and searches
/// the base address for the search token.
typedef int (*sparser_searchfunc_t)(__m256i, const char *);

// The callback fro the single parse function.
typedef bool (*sparser_callback_t)(const char *input);

typedef struct sparser_stats_ {
  // Number of times the search query matched.
  long total_matches;
  // Number of records sparser passed.
  long sparser_passed;
  // Number of records the callback passed by returning true.
  long callback_passed;
  // Total number of bytes we had to walk forward to see a new record,
  // when a match was found.
  long bytes_seeked_forward;
  // Total number of bytes we had to walk backward to see a new record,
  // when a match was found.
  long bytes_seeked_backward;
  // Fraction that sparser passed that the callback also passed
  double fraction_passed_correct;
  // Fraction of false positives.
  double fraction_passed_incorrect;
} sparser_stats_t;

/** Search for an 8-bit search string.
 *
 * @param reg the register filled with the search value
 * @param base the data to search. Should be at least 32 bytes long.
 *
 * @return the number of matches found.
 */
int search_epi8(__m256i reg, const char *base) {
  int count = 0;
  __m256i val = _mm256_loadu_si256((__m256i const *)(base));
  unsigned mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(reg, val));
  while (mask) {
    int index = ffs(mask) - 1;
    mask &= ~(1 << index);
    count++;
  }
  return count;
}

/** Search for an 16-bit search string.
 *
 * @param reg the register filled with the search value
 * @param base the data to search. Should be at least 32 bytes long.
 *
 * @return the number of matches found.
 */
int search_epi16(__m256i reg, const char *base) {
  int count = 0;
  __m256i val = _mm256_loadu_si256((__m256i const *)(base));
  unsigned mask = _mm256_movemask_epi8(_mm256_cmpeq_epi16(reg, val));
  mask &= 0x55555555;

  while (mask) {
    int index = ffs(mask) - 1;
    mask &= ~(1 << index);
    count++;
  }
  return count;
}

/** Search for an 32-bit search string.
 *
 * @param reg the register filled with the search value
 * @param base the data to search. Should be at least 32 bytes long.
 *
 * @return the number of matches found.
 */
int search_epi32(__m256i reg, const char *base) {
  int count = 0;
  __m256i val = _mm256_loadu_si256((__m256i const *)(base));
  unsigned mask = _mm256_movemask_epi8(_mm256_cmpeq_epi32(reg, val));
  mask &= 0x11111111;

  while (mask) {
    int index = ffs(mask) - 1;
    mask &= ~(1 << index);
    count++;
  }
  return count;
}

// add query and clip input
int sparser_add_query(sparser_query_t *query, const char *string) {
  if (query->count >= SPARSER_MAX_QUERY_COUNT) {
    return -1;
  }

  // Clip to the lowest multiple of 2.
  size_t len = (strnlen(string, SPARSER_MAX_QUERY_LENGTH + 1) / 2) * 2;
  if (len != 1 && len != 2 && len != 4) {
    return 1;
  }

  strncpy(query->queries[query->count], string, len);
  query->queries[query->count][len] = '\0';

  query->lens[query->count] = len;
  query->count++;
  return 0;
}

/* Performs the sparser search given a single search query and a buffer.
 *
 * This performs a simple sparser search given the query and input buffer. It
 * only searches for one occurance of the query string in each record. Records
 * are assumed to be delimited by newline.
 *
 * @param input the buffer to search
 * @param query the query to look for
 * @param length the size of the input buffer, in bytes.
 * @param query_length the size of the query string, in bytes.
 *
 * @return statistics about the run.
 * */
sparser_stats_t *sparser_search(char *input, long length,
                                sparser_query_t *query,
                                sparser_callback_t callback) {

  sparser_searchfunc_t searchfuncs[SPARSER_MAX_QUERY_COUNT];
  __m256i reg[SPARSER_MAX_QUERY_COUNT];

  sparser_stats_t stats;
  memset(&stats, 0, sizeof(stats));

  for (int i = 0; i < query->count; i++) {
    char *string = query->queries[i];
    printf("Set string %s (index=%d, len=%zu)\n", string, i, query->lens[i]);
    switch (query->lens[i]) {
    case 1: {
      searchfuncs[i] = search_epi8;
      uint8_t x = *((uint8_t *)string);
      reg[i] = _mm256_set1_epi8(x);
      break;
    }
    case 2: {
      searchfuncs[i] = search_epi16;
      uint16_t x = *((uint16_t *)string);
      reg[i] = _mm256_set1_epi16(x);
      break;
    }
    case 4: {
      searchfuncs[i] = search_epi32;
      uint32_t x = *((uint32_t *)string);
      reg[i] = _mm256_set1_epi32(x);
      break;
    }
    default: { return NULL; }
    }
  }

  // Bitmask designating which filters matched.
  // Bit i is set if if the ith filter matched for the current record.
  unsigned matchmask = 0;

  char *endptr = strchr(input, '\n');
  long end;
  if (endptr) {
    end = endptr - input;
  } else {
    end = length;
  }

  for (long i = 0; i < length; i += VECSZ) {

    if (i > end) {
      char *endptr = strchr(input + i, '\n');
      if (endptr) {
        end = endptr - input;
      } else {
        end = length;
      }
      matchmask = 0;
    }

    // Check each query.
    for (int j = 0; j < query->count; j++) {
      // Found this already.
      if (SET(matchmask, j)) {
        continue;
      }

      __m256i comparator = reg[j];
      int shifts = query->lens[j];
      sparser_searchfunc_t f = searchfuncs[j];

      for (int k = 0; k < shifts; k++) {
        // Returns the number of matches.
        int matched = f(comparator, input + i + k);
        if (matched > 0) {
          stats.total_matches += matched;
          // record that this query matched.
          matchmask |= (1 << j);
          // no need to check remaining shifts.

          // Debug
          /*
          char a = input[i + k + VECSZ];
          input[i + k + VECSZ] = '\0';
          printf("%s in %s\n", query->queries[j], input + i + k);
          input[i + k + VECSZ] = a;
          */
          break;
        }
      }
    }

    unsigned allset = ((1u << query->count) - 1u);
    // check if all the filters matched by checking if all the bits
    // necessary were set in matchmask.
    if ((matchmask & allset) == allset) {
      stats.sparser_passed++;

      // update start.
      long start = i;
      for (; start > 0 && input[start] != '\n'; start--)
        ;

      // Pass the current line to a full parser.
      char a = input[end];
      input[end] = '\0';
      if (callback(input + start)) {
        stats.callback_passed++;
      }
      input[end] = a;

      // Reset record level state.
      matchmask = 0;

      // Done with this record - move on to the next one.
      i = end + 1 - VECSZ;
    }
  }

  if (stats.sparser_passed > 0) {
    stats.fraction_passed_correct =
        (double)stats.callback_passed / (double)stats.sparser_passed;
    stats.fraction_passed_incorrect = 1.0 - stats.fraction_passed_correct;
  }

  sparser_stats_t *ret = (sparser_stats_t *)malloc(sizeof(sparser_stats_t));
  memcpy(ret, &stats, sizeof(stats));

  return ret;
}

static char *sparser_format_stats(sparser_stats_t *stats) {
  static char buf[8192];

  snprintf(buf, sizeof(buf), "Distinct Query matches: %ld\n\
Sparser Passed Records: %ld\n\
Callback Passed Records: %ld\n\
Bytes Seeked Forward: %ld\n\
Bytes Seeked Backward: %ld\n\
Fraction Passed Correctly: %f\n\
Fraction False Positives: %f",
           stats->total_matches, stats->sparser_passed, stats->callback_passed,
           stats->bytes_seeked_forward, stats->bytes_seeked_backward,
           stats->fraction_passed_correct, stats->fraction_passed_incorrect);
  return buf;
}

#endif