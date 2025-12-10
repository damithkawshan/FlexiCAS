#ifndef FLEXICAS_CACHE_CONFIG_H
#define FLEXICAS_CACHE_CONFIG_H

#define CACHE_LINE_SIZE 64

// L1 configuration
#define L1IW 4
#define L1WN 8

// L2 configuration
#define L2IW 5
#define L2WN 8

enum CacheTypeValue {
	BL = 0,
	DB = 2,
	SB = 1
};

#define CACHE_TYPE_BL 0
#define CACHE_TYPE_DB 2
#define CACHE_TYPE_SB 1

#define CACHE_TYPE CACHE_TYPE_SB

static_assert(static_cast<int>(CacheTypeValue::BL) == CACHE_TYPE_BL, "Cache type macro mismatch");
static_assert(static_cast<int>(CacheTypeValue::DB) == CACHE_TYPE_DB, "Cache type macro mismatch");
static_assert(static_cast<int>(CacheTypeValue::SB) == CACHE_TYPE_SB, "Cache type macro mismatch");

static inline constexpr CacheTypeValue cache_type_value() {
	return static_cast<CacheTypeValue>(CACHE_TYPE);
}

static inline constexpr const char* cache_type_suffix() {
	switch (cache_type_value()) {
	case BL:
		return "BL";
	case DB:
		return "DB";
	case SB:
		return "SB";
	default:
		return "UNKNOWN";
	}
}


#endif // FLEXICAS_CACHE_CONFIG_H
