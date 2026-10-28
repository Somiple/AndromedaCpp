#pragma once
#include <cmath>

namespace andromeda::util::math {

template <typename T>
class Vector2 {
public:
	Vector2() = default;
	Vector2(T xy) : x(xy), y(xy) {}
	Vector2(T x, T y) : x(x), y(y) {}

	Vector2 operator+(const Vector2& other) const {
		return { x + other.x, y + other.y };
	}

	Vector2 operator-(const Vector2& other) const {
		return { x - other.x, y - other.y };
	}

	// dot product of 2 vectors
	T operator*(const Vector2& other) const {
		return x * other.x + y * other.y;
	}

	T cross(const Vector2& other) const {
		return x * other.y - y * other.x;
	}

	T distance(const Vector2& from) const {
		Vector2 diff = *this - from;
		return std::sqrt(diff * diff);
	}
	
	T x{};
	T y{};
};

}