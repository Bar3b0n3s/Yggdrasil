#!/usr/bin/env python3
"""Generate the correctly rounded DetMath reference cases (Docs/Decisions/0007-detmath-reference-oracle.md).

Writes Tests/Source/Engine/Core/DetMathReferenceData.h, the oracle of the test "DetMath: within 1 ULP of correctly
rounded references over seeded tables". For every DetMath function (Sin, Cos, Tan, ASin, ACos, ATan, ATan2, Sinh, Cosh,
Tanh, Exp, Log, Log10 and Pow; SinCos is checked against the Sin and Cos tables) and for float and double, it holds
seeded inputs and the exact mathematical result rounded to nearest, ties to even, in the input's format. A float result
is rounded from the exact value directly, never through double.

How each reference is exact:
  - Inputs are IEEE values, so each is a dyadic rational; it becomes a Decimal through integer arithmetic only.
  - Each function is evaluated with the decimal module at a working precision of P digits, together with a rigorous
    bound on the absolute error, accumulated with upward rounding. exp, ln, log10 and sqrt are correctly rounded in the
    decimal module; sine, cosine, arc tangent and small hyperbolic sines are Taylor series with explicit truncation and
    rounding bounds.
  - Trigonometric arguments are reduced by k pi/2 with pi known to more places than the argument has integer digits
    plus P, and the subtraction is exact, so the reduction is right for every double up to 1.8e308 (the job the exact
    integer Payne-Hanek reduction does in DetMath). pi comes from Machin's formula in integer arithmetic.
  - Both ends of the error interval are rounded to binary64 or binary32 by an exact rational rounding routine. When
    they round differently, P doubles (Ziv's strategy) up to the last working precision; a case that is still undecided
    stops the generator.
  - Exact results on a rounding boundary never come out of an interval, so they are recognized first and computed with
    rational arithmetic: the zeros log 1, log10 1 and acos 1, log10 of the powers of ten, and every rational power
    (integer exponents, and dyadic exponents of perfect powers, such as 2^-1075, which ties to 0).

The inputs come from SplitMix64, seeded per region from SEED, and every value is formed and rounded with integer
arithmetic, so the output is byte-for-byte identical on every platform and Python 3.10+. The C++ test draws no inputs;
it reads the committed tables. Before generating, the script runs its self-test: the rounding routine against round
trips, ties, the subnormal range and overflow (and against the host's own conversions), pi's leading digits, the
recognized exact cases, and the independently computed correctly rounded values that DetMathTests.cpp commits.

Regenerate after changing the regions or the generator: `python Scripts/GenerateDetMathReference.py` (about a minute).
`--check` compares a fresh run with the committed file and writes nothing; `--self-test` runs only the self-test.

Exit codes: 0 success, 1 the self-test failed, a reference could not be decided or --check found a difference,
2 usage error.
"""

from __future__ import annotations

import platform
import sys

if sys.version_info < (3, 10):
    sys.exit(f"GenerateDetMathReference.py requires Python 3.10 or newer (this is Python {platform.python_version()})")

import argparse
import dataclasses
import decimal
import hashlib
import math
import struct
import textwrap
import time
from decimal import Decimal
from fractions import Fraction
from pathlib import Path
from typing import Callable, Sequence, Union

from Lib import paths
from Lib.report import EXIT_SUCCESS, Console, Status, Step, configure_stdio, emit_json, overall_exit_code

OUTPUT_PATH = paths.REPOSITORY_ROOT / "Tests" / "Source" / "Engine" / "Core" / "DetMathReferenceData.h"
SEED = 0xD37A7_2026_1006

MASK64 = (1 << 64) - 1
# Working precisions in decimal digits, tried in order until both ends of the error interval round alike.
WORKING_DIGITS = (40, 80, 160, 320, 640, 1280)
# Digits carried beyond the working precision inside every evaluation.
GUARD_DIGITS = 10
# pi is computed once to this many places: enough for the largest reduction (1280 + 10 + 308 + 30 places).
PI_PLACES = 1700
PI_LEADING_DIGITS = "3.14159265358979323846264338327950288419716939937510582097494459230781640628620899"
# Decimal exponent range of every context; results stay far inside it (the smallest is about 1e-650).
EXPONENT_LIMIT = 10**6
# Precision of the error-bound arithmetic, which rounds upward (downward for quantities that must not grow).
BOUND_DIGITS = 12
# |y log x| beyond which x^y is outside every format: e^1000 overflows and e^-1000 underflows to zero.
POWER_LOG_LIMIT = 1000
# A Taylor series stops at the first term below this fraction of one unit of the working precision.
SERIES_STOP = Decimal("0.001")
# Self-test sample sizes.
SELF_TEST_SAMPLES = 3000

ZERO = Decimal(0)
ONE = Decimal(1)
TWO = Decimal(2)

UP = decimal.Context(prec=BOUND_DIGITS, rounding=decimal.ROUND_CEILING, Emin=-EXPONENT_LIMIT, Emax=EXPONENT_LIMIT)
DOWN = decimal.Context(prec=BOUND_DIGITS, rounding=decimal.ROUND_FLOOR, Emin=-EXPONENT_LIMIT, Emax=EXPONENT_LIMIT)
# The thread context during evaluation: an operation that falls back to it must be exact.
EXACT_ONLY = decimal.Context(
    prec=BOUND_DIGITS,
    Emin=-EXPONENT_LIMIT,
    Emax=EXPONENT_LIMIT,
    traps=[decimal.Inexact, decimal.InvalidOperation, decimal.DivisionByZero, decimal.Overflow],
)

# The inputs of one table row (IEEE bit patterns), and a row: inputs and the expected bit pattern.
Inputs = tuple[int, ...]
Row = tuple[Inputs, int]
Number = Union[int, Fraction]


def power_of_two(exponent: int) -> Fraction:
    return Fraction(2) ** exponent


# --------------------------------------------------------------------------------------------------------------------
# Deterministic inputs
# --------------------------------------------------------------------------------------------------------------------


class SplitMix64:
    """SplitMix64 (Steele, Lea and Flood, 2014): a fully specified 64-bit sequence, identical on every platform."""

    def __init__(self, seed: int) -> None:
        self.state = seed & MASK64

    def next(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK64
        value = self.state
        value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
        value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & MASK64
        return value ^ (value >> 31)

    def below(self, bound: int) -> int:
        """Uniform in [0, bound) for 0 < bound <= 2^64, by rejection (no modulo bias)."""
        limit = ((1 << 64) // bound) * bound
        while True:
            value = self.next()
            if value < limit:
                return value % bound

    def unit(self) -> Fraction:
        """Uniform in [0, 1) with 53 random bits."""
        return Fraction(self.next() >> 11, 1 << 53)

    def coin(self) -> bool:
        return (self.next() >> 63) != 0


def region_seed(table: str, index: int) -> int:
    """A seed per region, so editing one region leaves the inputs of every other region unchanged."""
    digest = hashlib.sha256(f"{SEED:#x}/{table}/{index}".encode("ascii")).digest()
    return int.from_bytes(digest[:8], "little")


# --------------------------------------------------------------------------------------------------------------------
# IEEE formats and exact rounding
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Format:
    """An IEEE-754 binary interchange format."""

    name: str  # the C++ type: "float" or "double"
    title: str  # the table name suffix: "Float" or "Double"
    bits: int  # storage width
    precision: int  # significand bits, the implicit bit included
    max_exponent: int

    @property
    def min_exponent(self) -> int:
        return 1 - self.max_exponent

    @property
    def bits_type(self) -> str:
        return f"uint{self.bits}_t"

    @property
    def sign_bit(self) -> int:
        return 1 << (self.bits - 1)

    @property
    def infinity_bits(self) -> int:
        return (2 * self.max_exponent + 1) << (self.precision - 1)

    @property
    def max_finite_bits(self) -> int:
        return self.infinity_bits - 1

    @property
    def max_finite(self) -> Fraction:
        return Fraction((1 << self.precision) - 1) * power_of_two(self.max_exponent - self.precision + 1)

    @property
    def min_subnormal(self) -> Fraction:
        return power_of_two(self.min_exponent - self.precision + 1)

    @property
    def min_normal(self) -> Fraction:
        return power_of_two(self.min_exponent)

    def hex(self, bits: int) -> str:
        return f"0x{bits:0{self.bits // 4}x}"

    def is_finite(self, bits: int) -> bool:
        return (bits & ~self.sign_bit) < self.infinity_bits

    def decode(self, bits: int) -> tuple[int, int, int]:
        """(sign, significand, exponent) with value = (-1)^sign * significand * 2^exponent, for finite bits."""
        if not self.is_finite(bits):
            raise ValueError(f"{self.hex(bits)} is not a finite {self.name}")
        fraction_bits = self.precision - 1
        sign = bits >> (self.bits - 1)
        biased = (bits >> fraction_bits) & (2 * self.max_exponent + 1)
        significand = bits & ((1 << fraction_bits) - 1)
        if biased == 0:
            return sign, significand, self.min_exponent - fraction_bits
        return sign, significand | (1 << fraction_bits), biased - self.max_exponent - fraction_bits

    def to_fraction(self, bits: int) -> Fraction:
        sign, significand, exponent = self.decode(bits)
        value = Fraction(significand) * power_of_two(exponent)
        return -value if sign else value

    def to_decimal(self, bits: int) -> Decimal:
        """The exact value as a Decimal, since significand * 2^-k = significand * 5^k * 10^-k; the constructor never
        rounds."""
        sign, significand, exponent = self.decode(bits)
        text = str(significand << exponent) if exponent >= 0 else f"{significand * 5**-exponent}E{exponent}"
        return Decimal(("-" if sign else "") + text)

    def round(self, value: Fraction) -> int:
        """The bit pattern of `value` rounded to nearest, ties to even, with gradual underflow and overflow to infinity.
        A value that rounds to zero keeps its sign."""
        sign = self.sign_bit if value < 0 else 0
        magnitude = -value if value < 0 else value
        if magnitude == 0:
            return sign
        numerator, denominator = magnitude.numerator, magnitude.denominator
        # Find the exponent with 2^exponent <= magnitude < 2^(exponent + 1).
        exponent = numerator.bit_length() - denominator.bit_length()
        if exponent >= 0:
            below = numerator < (denominator << exponent)
        else:
            below = (numerator << -exponent) < denominator
        if below:
            exponent -= 1
        # Below the normal range the quantum stays at the subnormal spacing.
        exponent = max(exponent, self.min_exponent)
        shift = self.precision - 1 - exponent
        if shift >= 0:
            scaled_numerator, scaled_denominator = numerator << shift, denominator
        else:
            scaled_numerator, scaled_denominator = numerator, denominator << -shift
        significand, remainder = divmod(scaled_numerator, scaled_denominator)
        twice = 2 * remainder
        if twice > scaled_denominator or (twice == scaled_denominator and significand & 1):
            significand += 1
        if significand == 1 << self.precision:
            significand >>= 1
            exponent += 1
        implicit = 1 << (self.precision - 1)
        if significand < implicit:
            return sign | significand
        if exponent > self.max_exponent:
            return sign | self.infinity_bits
        return sign | ((exponent + self.max_exponent) << (self.precision - 1)) | (significand - implicit)


FLOAT = Format("float", "Float", 32, 24, 127)
DOUBLE = Format("double", "Double", 64, 53, 1023)
FORMATS = (FLOAT, DOUBLE)


# --------------------------------------------------------------------------------------------------------------------
# Rigorous high-precision evaluation
# --------------------------------------------------------------------------------------------------------------------


def compute_pi(places: int) -> Decimal:
    """pi to `places` decimal places, from Machin's formula pi = 16 atan(1/5) - 4 atan(1/239) in integers scaled by
    10^(places + 10). Each of the fewer than 4 * places truncations loses less than one unit, so the error is below
    10^-(places + 5)."""
    scale = 10 ** (places + 10)

    def inverse_arc_tangent(denominator: int) -> int:
        total = 0
        power = scale // denominator
        square = denominator * denominator
        index = 0
        while power:
            term = power // (2 * index + 1)
            total += -term if index % 2 else term
            power //= square
            index += 1
        return total

    value = 16 * inverse_arc_tangent(5) - 4 * inverse_arc_tangent(239)
    return Decimal(f"{value}E-{places + 10}")


@dataclasses.dataclass(frozen=True)
class Approximation:
    """A value and a bound on its distance to the exact result; an error of 0 means the value is exact."""

    value: Union[Decimal, Fraction]
    error: Decimal

    def __neg__(self) -> Approximation:
        value = self.value.copy_negate() if isinstance(self.value, Decimal) else -self.value
        return Approximation(value, self.error)


UNDECIDED = Approximation(ZERO, Decimal("Infinity"))


def up_sum(*terms: Decimal) -> Decimal:
    total = ZERO
    for term in terms:
        total = UP.add(total, term)
    return total


def up_product(*factors: Union[Decimal, int]) -> Decimal:
    product = ONE
    for factor in factors:
        product = UP.multiply(product, Decimal(factor))
    return product


def relative_bound(value: Decimal, error: Decimal) -> Decimal | None:
    """A bound of |value - exact| / |exact|, or None when the approximation does not keep |exact| well away from 0."""
    lower = DOWN.subtract(value.copy_abs(), error)
    if lower <= 0 or UP.multiply(error, TWO) > lower:
        return None
    return UP.divide(error, lower)


class Evaluator:
    """Every DetMath function at one working precision, each result with a rigorous absolute error bound.

    Each decimal operation, exp, ln, log10 and sqrt included, is correctly rounded, so it adds at most
    unit * |result| (unit = 10^(1 - precision), one unit in the last place at most). Inputs are exact. Bounds are
    accumulated with upward rounding and doubled at the end of each step, which also covers using computed instead of
    exact magnitudes inside the bounds."""

    def __init__(self, digits: int, pi: Decimal) -> None:
        precision = digits + GUARD_DIGITS
        self.context = decimal.Context(
            prec=precision,
            rounding=decimal.ROUND_HALF_EVEN,
            Emin=-EXPONENT_LIMIT,
            Emax=EXPONENT_LIMIT,
            traps=[decimal.InvalidOperation, decimal.DivisionByZero, decimal.Overflow],
        )
        self.unit = Decimal(f"1E{1 - precision}")
        self.exact_half_pi = decimal.Context(prec=PI_PLACES + 20).divide(pi, TWO)
        # pi to precision + 5 places (the master's own error is far smaller), and its exact half.
        self.pi = pi.quantize(Decimal(f"1E-{precision + 5}"), context=decimal.Context(prec=precision + 10))
        self.pi_error = Decimal(f"1E-{precision + 5}")
        self.half_pi = decimal.Context(prec=precision + 20).divide(self.pi, TWO)
        self.half_pi_error = self.pi_error

    def rounding(self, value: Decimal) -> Decimal:
        """The error bound of the one correctly rounded operation that produced `value`."""
        return up_product(value.copy_abs(), self.unit)

    def series(self, first: Decimal, ratio: Callable[[Decimal, int], Decimal], alternating: bool) -> Approximation:
        """first + t(1) + t(2) + ... with t(n) = ratio(t(n-1), n). Each step rounds at most 4 times, so t(n) is within
        (4n + 1) units of its exact value, and each addition adds one unit of the running sum: the rounding error is
        below unit * sum|t| * (6n + 2). The tail is below the first omitted term for an alternating series with
        decreasing terms, and below twice it for positive terms whose ratio is below 1/2."""
        with decimal.localcontext(self.context):
            term = +first
            total = term
            magnitude = abs(term)
            index = 0
            while True:
                index += 1
                term = ratio(term, index)
                if abs(term) <= abs(total) * self.unit * SERIES_STOP:
                    break
                total += term
                magnitude += abs(term)
        tail = term.copy_abs() if alternating else up_product(term.copy_abs(), 2)
        error = up_sum(up_product(magnitude, self.unit, 6 * index + 2), tail)
        return Approximation(total, up_product(error, 2))

    def quotient(self, numerator: Approximation, denominator: Approximation) -> Approximation:
        assert isinstance(numerator.value, Decimal) and isinstance(denominator.value, Decimal)
        numerator_relative = relative_bound(numerator.value, numerator.error)
        denominator_relative = relative_bound(denominator.value, denominator.error)
        if numerator_relative is None or denominator_relative is None:
            return UNDECIDED
        value = self.context.divide(numerator.value, denominator.value)
        relative = up_sum(numerator_relative, denominator_relative, self.unit)
        return Approximation(value, up_product(value.copy_abs(), relative, 2))

    # Trigonometric functions

    def reduce(self, x: Decimal) -> tuple[int, Decimal, Decimal]:
        """(k, r, error) with x = k pi/2 + r: |r| < 0.8, and the computed r within `error` of the exact one."""
        if x.copy_abs() < Decimal("0.78"):
            return 0, x, ZERO
        exponent = max(0, x.adjusted())
        places = self.context.prec + exponent + 30
        if places > PI_PLACES - 10:
            raise ValueError(f"reducing {x} needs pi to {places} places")
        half_pi = self.exact_half_pi.quantize(Decimal(f"1E-{places}"), context=decimal.Context(prec=places + 10))
        quotient = decimal.Context(prec=exponent + 25).divide(x, half_pi)
        multiple = int(quotient.to_integral_value(rounding=decimal.ROUND_HALF_EVEN))
        # x has len(digits) digits and half_pi `places` places, so this context holds the difference exactly; the
        # Inexact trap proves it.
        exact = decimal.Context(prec=len(x.as_tuple().digits) + places + exponent + 20, traps=[decimal.Inexact])
        r = exact.subtract(x, exact.multiply(Decimal(multiple), half_pi))
        if r.copy_abs() >= Decimal("0.8"):
            raise ValueError(f"the reduction of {x} left {r}")
        # |half_pi - pi/2| <= 10^-places / 2 plus the far smaller error of the master pi.
        return multiple, r, up_product(abs(multiple), Decimal(f"1E-{places}"))

    def sin_cos(self, x: Decimal) -> tuple[Approximation, Approximation]:
        multiple, r, reduction_error = self.reduce(x)
        square = self.context.multiply(r, r)
        sine = self.series(r, lambda term, n: -term * square / ((2 * n) * (2 * n + 1)), alternating=True)
        cosine = self.series(ONE, lambda term, n: -term * square / ((2 * n - 1) * (2 * n)), alternating=True)
        # |sin'| and |cos'| are at most 1, so the reduction error adds as it is.
        sine = Approximation(sine.value, up_sum(sine.error, reduction_error))
        cosine = Approximation(cosine.value, up_sum(cosine.error, reduction_error))
        quadrant = multiple % 4
        return (sine, cosine, -sine, -cosine)[quadrant], (cosine, -sine, -cosine, sine)[quadrant]

    def sin(self, x: Decimal) -> Approximation:
        return self.sin_cos(x)[0]

    def cos(self, x: Decimal) -> Approximation:
        return self.sin_cos(x)[1]

    def tan(self, x: Decimal) -> Approximation:
        sine, cosine = self.sin_cos(x)
        return self.quotient(sine, cosine)

    # Arc tangent and the functions built on it

    def arc_tangent_unit(self, t: Decimal, relative: Decimal) -> Approximation:
        """atan of the exact value that 0 <= t <= 1 approximates within `relative` relative error."""
        # Two halvings atan t = 2 atan(t / (1 + sqrt(1 + t^2))) bring t <= 1 below tan(pi/16) < 0.2. Each one adds at
        # most 4 units of relative error, and its relative condition number is below 1.
        halvings = 0 if t <= Decimal("0.2") else 2
        reduced = t
        with decimal.localcontext(self.context):
            for _ in range(halvings):
                reduced = reduced / (1 + (1 + reduced * reduced).sqrt())
        reduced_relative = up_sum(relative, up_product(self.unit, 4 * halvings))
        square = self.context.multiply(reduced, reduced)
        # t(n) = (-1)^n t^(2n+1) / (2n+1) from t(n-1): times -t^2 (2n-1) / (2n+1).
        series = self.series(reduced, lambda term, n: -term * square * (2 * n - 1) / (2 * n + 1), alternating=True)
        assert isinstance(series.value, Decimal)
        scale = 1 << halvings
        value = self.context.multiply(series.value, scale)
        # |atan'| <= 1, so the error of the reduced argument passes through at most as it is.
        propagated = up_product(reduced_relative, reduced.copy_abs(), 2)
        error = up_sum(up_product(scale, up_sum(series.error, propagated)), self.rounding(value))
        return Approximation(value, error)

    def arc_tangent(self, t: Decimal, relative: Decimal = ZERO) -> Approximation:
        """atan of the exact value that `t` approximates within `relative` relative error."""
        if t < 0:
            return -self.arc_tangent(t.copy_negate(), relative)
        if t <= 1:
            return self.arc_tangent_unit(t, relative)
        # atan t = pi/2 - atan(1/t); 1/t has the relative error of t plus one rounding.
        inner = self.arc_tangent_unit(self.context.divide(ONE, t), up_sum(relative, self.unit))
        assert isinstance(inner.value, Decimal)
        value = self.context.subtract(self.half_pi, inner.value)
        return Approximation(value, up_sum(inner.error, self.half_pi_error, self.rounding(value)))

    def atan(self, x: Decimal) -> Approximation:
        return self.arc_tangent(x)

    def asin(self, x: Decimal) -> Approximation:
        magnitude = x.copy_abs()
        if magnitude == 1:
            value = self.context.plus(self.half_pi)
            result = Approximation(value, up_sum(self.half_pi_error, self.rounding(value)))
        else:
            # asin a = atan(a / sqrt((1 - a)(1 + a))): 1 - a and 1 + a (1 unit each), the product (3), the root (2.5)
            # and the quotient (3.5) stay below 4 units relative.
            with decimal.localcontext(self.context):
                quotient = magnitude / ((1 - magnitude) * (1 + magnitude)).sqrt()
            result = self.arc_tangent(quotient, up_product(self.unit, 4))
        return -result if x < 0 else result

    def acos(self, x: Decimal) -> Approximation:
        if x == 1:
            return Approximation(ZERO, ZERO)
        if x == -1:
            value = self.context.plus(self.pi)
            return Approximation(value, up_sum(self.pi_error, self.rounding(value)))
        # acos x = 2 atan(sqrt((1 - x) / (1 + x))): below 3 units relative before the arc tangent.
        with decimal.localcontext(self.context):
            root = ((1 - x) / (1 + x)).sqrt()
        angle = self.arc_tangent(root, up_product(self.unit, 3))
        assert isinstance(angle.value, Decimal)
        value = self.context.multiply(angle.value, 2)
        return Approximation(value, up_sum(up_product(angle.error, 2), self.rounding(value)))

    def atan2(self, y: Decimal, x: Decimal) -> Approximation:
        """The angle of (x, y) for finite nonzero x and y."""
        angle = self.arc_tangent(self.context.divide(y.copy_abs(), x.copy_abs()), self.unit)
        if x < 0:
            assert isinstance(angle.value, Decimal)
            value = self.context.subtract(self.pi, angle.value)
            angle = Approximation(value, up_sum(angle.error, self.pi_error, self.rounding(value)))
        return -angle if y < 0 else angle

    # Hyperbolic functions

    def sinh(self, x: Decimal) -> Approximation:
        magnitude = x.copy_abs()
        if magnitude < 1:
            square = self.context.multiply(magnitude, magnitude)
            # Positive terms with ratio a^2 / ((2n)(2n+1)) < 1/6.
            result = self.series(magnitude, lambda term, n: term * square / ((2 * n) * (2 * n + 1)), alternating=False)
        else:
            plus, minus = self.context.exp(magnitude), self.context.exp(magnitude.copy_negate())
            difference = self.context.subtract(plus, minus)
            value = self.context.divide(difference, TWO)
            error = up_sum(up_product(up_sum(plus, minus, difference), self.unit), self.rounding(value))
            result = Approximation(value, up_product(error, 2))
        return -result if x < 0 else result

    def cosh(self, x: Decimal) -> Approximation:
        plus, minus = self.context.exp(x.copy_abs()), self.context.exp(x.copy_abs().copy_negate())
        total = self.context.add(plus, minus)
        value = self.context.divide(total, TWO)
        error = up_sum(up_product(up_sum(plus, minus, total), self.unit), self.rounding(value))
        return Approximation(value, up_product(error, 2))

    def tanh(self, x: Decimal) -> Approximation:
        magnitude = x.copy_abs()
        if magnitude < 1:
            result = self.quotient(self.sinh(magnitude), self.cosh(magnitude))
        else:
            plus, minus = self.context.exp(magnitude), self.context.exp(magnitude.copy_negate())
            difference = self.context.subtract(plus, minus)
            total = self.context.add(plus, minus)
            difference_error = up_product(up_sum(plus, minus, difference), self.unit)
            total_error = up_product(up_sum(plus, minus, total), self.unit)
            result = self.quotient(Approximation(difference, difference_error), Approximation(total, total_error))
        return -result if x < 0 else result

    # Exponential, logarithms and power

    def exp(self, x: Decimal) -> Approximation:
        value = self.context.exp(x)
        return Approximation(value, self.rounding(value))

    def log(self, x: Decimal) -> Approximation:
        if x == 1:
            return Approximation(ZERO, ZERO)
        value = self.context.ln(x)
        return Approximation(value, self.rounding(value))

    def log10(self, x: Decimal) -> Approximation:
        exponent = power_of_ten_exponent(Fraction(x))
        if exponent is not None:
            return Approximation(Decimal(exponent), ZERO)
        value = self.context.log10(x)
        return Approximation(value, self.rounding(value))

    def pow(self, base: Decimal, exponent: Decimal) -> Approximation:
        """base^exponent for finite nonzero arguments; a negative base only with an integer exponent."""
        exact = exact_power(Fraction(base), Fraction(exponent))
        if exact is not None:
            return Approximation(exact, ZERO)
        if base < 0 and exponent != exponent.to_integral_value():
            raise ValueError(f"pow({base}, {exponent}) is not real")
        negative = base < 0 and int(exponent) % 2 == 1
        product = self.context.multiply(exponent, self.context.ln(base.copy_abs()))
        if product.copy_abs() > POWER_LOG_LIMIT:
            # Far outside every format: any value beyond the limit rounds to infinity or zero alike.
            beyond = Decimal("1E500") if product > 0 else Decimal("1E-500")
            return Approximation(beyond.copy_negate() if negative else beyond, ZERO)
        # The product is within 2 units relative of y log|x|, and e^(p + d) = e^p (1 + d + ...), so the exponential is
        # within one rounding plus about 2 |p| units relative (4 |p| leaves room for the higher-order terms).
        value = self.context.exp(product)
        relative = up_sum(self.unit, up_product(self.unit, product.copy_abs(), 4))
        result = Approximation(value, up_product(value, relative, 2))
        return -result if negative else result

    def evaluate(self, function: str, arguments: Sequence[Decimal]) -> Approximation:
        method: Callable[..., Approximation] = getattr(self, FUNCTIONS[function].method)
        return method(*arguments)


def power_of_ten_exponent(value: Fraction) -> int | None:
    """k when value is exactly 10^k for an integer k >= 0, else None."""
    if value.denominator != 1 or value <= 0:
        return None
    integer = value.numerator
    exponent = 0
    while integer % 10 == 0:
        integer //= 10
        exponent += 1
    return exponent if integer == 1 else None


def exact_root(value: int, halvings: int) -> int | None:
    """The integer 2^halvings-th root of `value` when it is exact, else None."""
    for _ in range(halvings):
        root = math.isqrt(value)
        if root * root != value:
            return None
        value = root
        if value == 1:
            return 1
    return value


def exact_power(base: Fraction, exponent: Fraction) -> Fraction | None:
    """base^exponent when it is rational and could lie on a rounding boundary, else None: then it is irrational, or a
    rational that equals no format value or midpoint, and an error interval decides its rounding.

    The base is a dyadic M 2^E with M odd and the exponent p / 2^k with p odd or k = 0. The power is rational exactly
    when 2^k divides E and M is a perfect 2^k-th power; then it is (M' 2^E')^p with M' the root. A boundary is a dyadic
    with at most precision + 1 significant bits, so for M' > 1 only 0 <= p <= 64 can reach one (the power has the odd
    factor M'^p), and for M' = 1 the power is the power of two 2^(E' p)."""
    numerator, denominator = abs(base).numerator, abs(base).denominator
    if denominator == 1:
        shift = (numerator & -numerator).bit_length() - 1
        odd = numerator >> shift
    else:
        odd, shift = numerator, -(denominator.bit_length() - 1)
    halvings = exponent.denominator.bit_length() - 1
    if halvings > 0:
        if shift % (1 << halvings) != 0:
            return None
        root = exact_root(odd, halvings)
        if root is None:
            return None
        odd, shift = root, shift >> halvings
    power = exponent.numerator
    sign = -1 if base < 0 and power % 2 else 1
    if odd == 1:
        if abs(shift * power) > 4 * POWER_LOG_LIMIT:
            return None
        return sign * power_of_two(shift * power)
    if not 0 <= power <= 64:
        return None
    return sign * (Fraction(odd) * power_of_two(shift)) ** power


class ReferenceUndecidedError(Exception):
    """A result lies too close to a rounding boundary for the last working precision."""


class ReferenceCalculator:
    """Correctly rounded results, by Ziv's strategy over the working precisions."""

    def __init__(self) -> None:
        self.pi = compute_pi(PI_PLACES)
        self.evaluators = [Evaluator(digits, self.pi) for digits in WORKING_DIGITS]
        self.escalations = 0

    def reference(self, function: str, fmt: Format, inputs: Inputs) -> int:
        arguments = [fmt.to_decimal(bits) for bits in inputs]
        for attempt, evaluator in enumerate(self.evaluators):
            # Every rounding must happen in a context whose error the bounds account for. Python's own abs(), unary
            # minus and unary plus round to the thread's context instead, so that context traps any inexact result.
            with decimal.localcontext(EXACT_ONLY):
                approximation = evaluator.evaluate(function, arguments)
            if approximation.error == 0:
                return fmt.round(Fraction(approximation.value))
            if approximation.error.is_finite():
                center = Fraction(approximation.value)
                radius = Fraction(approximation.error)
                low = fmt.round(center - radius)
                if low == fmt.round(center + radius):
                    self.escalations += 1 if attempt > 0 else 0
                    return low
        described = ", ".join(fmt.hex(bits) for bits in inputs)
        raise ReferenceUndecidedError(
            f"{function}({described}) in {fmt.name}: the rounding is not decided at {WORKING_DIGITS[-1]} digits"
        )


# --------------------------------------------------------------------------------------------------------------------
# Input regions
# --------------------------------------------------------------------------------------------------------------------


def describe_number(value: Number, fmt: Format) -> str:
    value = Fraction(value)
    sign = "-" if value < 0 else ""
    magnitude = abs(value)
    numerator, denominator = magnitude.numerator, magnitude.denominator
    if magnitude == fmt.max_finite:
        return f"{sign}max"
    if numerator & (numerator - 1) == 0 and denominator & (denominator - 1) == 0 and not 1 <= magnitude < 1024:
        return f"{sign}2^{numerator.bit_length() - denominator.bit_length()}"
    if denominator == 1 and numerator < 10**7:
        return f"{sign}{numerator}"
    return f"{sign}{float(magnitude)!r}"


def describe_range(low: Number, high: Number, fmt: Format) -> str:
    return f"[{describe_number(low, fmt)}, {describe_number(high, fmt)}]"


class Sampler:
    """Draws one input value of a format (a bit pattern)."""

    def sample(self, rng: SplitMix64, fmt: Format) -> int:
        raise NotImplementedError

    def describe(self, fmt: Format) -> str:
        raise NotImplementedError


def with_random_sign(bits: int, signed: bool, rng: SplitMix64, fmt: Format) -> int:
    return bits | fmt.sign_bit if signed and rng.coin() else bits


def offset_bits(center: int, rng: SplitMix64, ulps: int, fmt: Format) -> int:
    """A positive finite value within `ulps` units in the last place of the positive value `center`."""
    return min(max(center + rng.below(2 * ulps + 1) - ulps, 1), fmt.max_finite_bits)


@dataclasses.dataclass(frozen=True)
class UniformIn(Sampler):
    """Uniform in [low, high] (53 random bits), rounded to the format."""

    low: Number
    high: Number

    def sample(self, rng: SplitMix64, fmt: Format) -> int:
        low, high = Fraction(self.low), Fraction(self.high)
        return fmt.round(low + (high - low) * rng.unit())

    def describe(self, fmt: Format) -> str:
        return f"uniform in {describe_range(self.low, self.high, fmt)}"


@dataclasses.dataclass(frozen=True)
class Binades(Sampler):
    """Uniform over the format values between low > 0 and high, so every binade gets the same share; either sign when
    `signed`."""

    low: Number
    high: Number
    signed: bool = False

    def sample(self, rng: SplitMix64, fmt: Format) -> int:
        first = fmt.round(Fraction(self.low))
        last = min(fmt.round(Fraction(self.high)), fmt.max_finite_bits)
        return with_random_sign(first + rng.below(last - first + 1), self.signed, rng, fmt)

    def describe(self, fmt: Format) -> str:
        return f"{'+-' if self.signed else ''}{describe_range(self.low, self.high, fmt)} by binade"


@dataclasses.dataclass(frozen=True)
class Around(Sampler):
    """Within `ulps` units in the last place of one of `points`; either sign when `signed`."""

    points: tuple[Number, ...]
    ulps: int
    signed: bool = False

    def sample(self, rng: SplitMix64, fmt: Format) -> int:
        center = fmt.round(Fraction(self.points[rng.below(len(self.points))]))
        return with_random_sign(offset_bits(center, rng, self.ulps, fmt), self.signed, rng, fmt)

    def describe(self, fmt: Format) -> str:
        points = ", ".join(describe_number(point, fmt) for point in self.points)
        return f"within {self.ulps} ULP of {'+-' if self.signed else ''}{{{points}}}"


@dataclasses.dataclass(frozen=True)
class AroundOne(Sampler):
    """1 + d or 1 - d with d drawn by binade from [low, high]."""

    low: Number
    high: Number

    def sample(self, rng: SplitMix64, fmt: Format) -> int:
        distance = fmt.to_fraction(Binades(self.low, self.high).sample(rng, fmt))
        return fmt.round(1 + distance if rng.coin() else 1 - distance)

    def describe(self, fmt: Format) -> str:
        return f"1 +- d, d in {describe_range(self.low, self.high, fmt)} by binade"


@dataclasses.dataclass(frozen=True)
class OneMinus(Sampler):
    """+-(1 - d) with d drawn by binade from [low, high]: arguments near +-1."""

    low: Number
    high: Number

    def sample(self, rng: SplitMix64, fmt: Format) -> int:
        distance = fmt.to_fraction(Binades(self.low, self.high).sample(rng, fmt))
        return with_random_sign(fmt.round(1 - distance), True, rng, fmt)

    def describe(self, fmt: Format) -> str:
        return f"+-(1 - d), d in {describe_range(self.low, self.high, fmt)} by binade"


@dataclasses.dataclass(frozen=True)
class NearMultiples(Sampler):
    """Within `ulps` of k pi/2 for a nonzero integer k in [low, high]: arguments whose reduced value is tiny."""

    low: int
    high: int
    ulps: int

    def sample(self, rng: SplitMix64, fmt: Format) -> int:
        multiple = self.low + rng.below(self.high - self.low + 1)
        multiple = multiple if multiple != 0 else 1
        bits = offset_bits(fmt.round(abs(multiple) * HALF_PI), rng, self.ulps, fmt)
        return bits | fmt.sign_bit if multiple < 0 else bits

    def describe(self, fmt: Format) -> str:
        return f"within {self.ulps} ULP of k pi/2, k in [{self.low}, {self.high}]"


@dataclasses.dataclass(frozen=True)
class Integers(Sampler):
    """A uniform integer in [low, high]."""

    low: int
    high: int

    def sample(self, rng: SplitMix64, fmt: Format) -> int:
        return fmt.round(Fraction(self.low + rng.below(self.high - self.low + 1)))

    def describe(self, fmt: Format) -> str:
        return f"integers in [{self.low}, {self.high}]"


@dataclasses.dataclass(frozen=True)
class HalfIntegers(Sampler):
    """n + 1/2 for a uniform integer n in [low, high]."""

    low: int
    high: int

    def sample(self, rng: SplitMix64, fmt: Format) -> int:
        return fmt.round(Fraction(2 * (self.low + rng.below(self.high - self.low + 1)) + 1, 2))

    def describe(self, fmt: Format) -> str:
        return f"n + 1/2, n in [{self.low}, {self.high}]"


@dataclasses.dataclass(frozen=True)
class PerfectSquares(Sampler):
    """m^2 4^e for uniform integers m in [1, root_limit] and e in [-8, 8], exactly representable for the limits used."""

    root_limit: int

    def sample(self, rng: SplitMix64, fmt: Format) -> int:
        root = 1 + rng.below(self.root_limit)
        return fmt.round(Fraction(root * root) * power_of_two(2 * (rng.below(17) - 8)))

    def describe(self, fmt: Format) -> str:
        return f"perfect squares m^2 4^e, m <= {self.root_limit}"


class Region:
    """A group of inputs of one table, listed in the comment above the table."""

    def generate(self, rng: SplitMix64, fmt: Format) -> list[Inputs]:
        raise NotImplementedError

    def describe(self, fmt: Format) -> str:
        raise NotImplementedError


@dataclasses.dataclass(frozen=True)
class Cases(Region):
    """`count` random inputs, one sampler per argument."""

    count: int
    samplers: tuple[Sampler, ...]

    def generate(self, rng: SplitMix64, fmt: Format) -> list[Inputs]:
        return [tuple(sampler.sample(rng, fmt) for sampler in self.samplers) for _ in range(self.count)]

    def describe(self, fmt: Format) -> str:
        return f"{self.count} x {', '.join(sampler.describe(fmt) for sampler in self.samplers)}"


@dataclasses.dataclass(frozen=True)
class Neighbors(Region):
    """Every value within `ulps` of each point, both signs when `signed`."""

    points: tuple[Number, ...]
    ulps: int
    signed: bool = False

    def generate(self, rng: SplitMix64, fmt: Format) -> list[Inputs]:
        rows: list[Inputs] = []
        for point in self.points:
            center = fmt.round(Fraction(point))
            for bits in range(max(center - self.ulps, 1), min(center + self.ulps, fmt.max_finite_bits) + 1):
                rows.append((bits,))
                if self.signed:
                    rows.append((bits | fmt.sign_bit,))
        return rows

    def describe(self, fmt: Format) -> str:
        points = ", ".join(describe_number(point, fmt) for point in self.points)
        return f"every value within {self.ulps} ULP of {'+-' if self.signed else ''}{{{points}}}"


@dataclasses.dataclass(frozen=True)
class Listed(Region):
    """Edge cases, each argument rounded to the format."""

    rows: tuple[tuple[Number, ...], ...]

    def generate(self, rng: SplitMix64, fmt: Format) -> list[Inputs]:
        return [tuple(fmt.round(Fraction(value)) for value in row) for row in self.rows]

    def describe(self, fmt: Format) -> str:
        return f"{len(self.rows)} listed edge cases"


@dataclasses.dataclass(frozen=True)
class PowerTargets(Region):
    """Pow arguments that put x^y near e^t: x from `base`, t uniform in [low, high] and y = t / ln x, computed in a
    fixed 40-digit decimal context and rounded to the format."""

    count: int
    base: Sampler
    low: Number
    high: Number

    def generate(self, rng: SplitMix64, fmt: Format) -> list[Inputs]:
        context = decimal.Context(prec=40, rounding=decimal.ROUND_HALF_EVEN, Emin=-EXPONENT_LIMIT, Emax=EXPONENT_LIMIT)
        rows: list[Inputs] = []
        while len(rows) < self.count:
            base = self.base.sample(rng, fmt)
            target = Fraction(self.low) + (Fraction(self.high) - Fraction(self.low)) * rng.unit()
            if fmt.to_fraction(base) == 1:
                continue
            target_value = context.divide(Decimal(target.numerator), Decimal(target.denominator))
            exponent = context.divide(target_value, context.ln(fmt.to_decimal(base)))
            rows.append((base, fmt.round(Fraction(exponent))))
        return rows

    def describe(self, fmt: Format) -> str:
        return (f"{self.count} x (x {self.base.describe(fmt)}, y = t / ln x for t uniform in "
                f"{describe_range(self.low, self.high, fmt)})")


# pi/2 to 75 digits: only for choosing arguments near its multiples, never for a reference.
HALF_PI = Fraction("1.570796326794896619231321691639751442098584699687552910487472296153908203143")


def per_format(fmt: Format, double: Number, single: Number) -> Number:
    return double if fmt is DOUBLE else single


def tiny_regions(fmt: Format) -> list[Region]:
    """Arguments far below 1 (subnormals included) and around 2^-27, where DetMath returns x (or 1) directly."""
    return [
        Cases(100, (Binades(fmt.min_subnormal, power_of_two(-4), signed=True),)),
        Cases(30, (Binades(power_of_two(-29), power_of_two(-25), signed=True),)),
    ]


def trigonometric_regions(fmt: Format) -> list[Region]:
    edges: tuple[tuple[Number, ...], ...]
    if fmt is DOUBLE:
        # 1/2, RN(pi/2), RN(pi), arguments of the reduction tests, 2^1023 and max.
        edges = ((Fraction(1, 2),), (HALF_PI,), (2 * HALF_PI,), (10**6,), (3 * 10**9,), (10**22,), (10**100,),
                 (power_of_two(1023),), (DBL_MAX,))
    else:
        edges = ((Fraction(1, 2),), (HALF_PI,), (2 * HALF_PI,), (10**10,), (10**30,), (power_of_two(127),),
                 (FLT_MAX,))
    regions: list[Region] = [
        Cases(400, (UniformIn(-100, 100),)),
        *tiny_regions(fmt),
        Cases(150, (NearMultiples(-64, 64, 2),)),
        Cases(150, (NearMultiples(-(1 << 20), 1 << 20, 2),)),
        Cases(150, (Binades(100, power_of_two(19), signed=True),)),
        Cases(40, (Around((power_of_two(19),), 20, signed=True),)),
        Cases(250, (Binades(power_of_two(19), fmt.max_finite, signed=True),)),
        Listed(edges),
    ]
    if fmt is DOUBLE:
        # The double closest to a multiple of pi/2 (its cosine is about -4.7e-19), and its neighbours.
        regions.append(Neighbors((Fraction(6381956970095103) * power_of_two(797),), 3, signed=True))
    return regions


def inverse_sine_regions(fmt: Format) -> list[Region]:
    near_one = 1 - power_of_two(-fmt.precision)
    return [
        Cases(500, (UniformIn(-1, 1),)),
        *tiny_regions(fmt),
        Cases(200, (OneMinus(power_of_two(-fmt.precision), power_of_two(-4)),)),
        Cases(50, (Around((Fraction(1, 2),), 50, signed=True),)),
        # asin and acos switch between their two atan forms at 1/sqrt(2).
        Cases(50, (Around((Fraction(7071067811865476, 10**16),), 50, signed=True),)),
        Listed(((1,), (-1,), (Fraction(1, 2),), (Fraction(-1, 2),), (near_one,), (-near_one,), (fmt.min_subnormal,))),
    ]


def arc_tangent_regions(fmt: Format) -> list[Region]:
    return [
        Cases(400, (UniformIn(-1000, 1000),)),
        Cases(250, (UniformIn(-1, 1),)),
        *tiny_regions(fmt),
        Cases(100, (AroundOne(power_of_two(-fmt.precision), power_of_two(-4)),)),
        Cases(150, (Binades(1000, fmt.max_finite, signed=True),)),
        Cases(30, (Around((power_of_two(60),), 8, signed=True),)),
        Listed(((1,), (-1,), (power_of_two(60),), (fmt.max_finite,), (-fmt.max_finite,), (fmt.min_subnormal,))),
    ]


def arc_tangent2_regions(fmt: Format) -> list[Region]:
    tiny, huge = fmt.min_subnormal, fmt.max_finite
    return [
        Cases(500, (UniformIn(-100, 100), UniformIn(-100, 100))),
        Cases(200, (Binades(tiny, huge, signed=True), Binades(tiny, huge, signed=True))),
        Cases(100, (Binades(Fraction(1, 1000), 1000, signed=True), Binades(Fraction(1, 1000), 1000, signed=True))),
        Cases(100, (Around((1,), 64, signed=True), Around((1,), 64, signed=True))),
        Cases(100, (UniformIn(-1, 1), UniformIn(-per_format(fmt, 10**300, 10**30), -1000))),
        Cases(50, (Binades(tiny, fmt.min_normal, signed=True), Binades(tiny, fmt.min_normal, signed=True))),
        Listed(((1, 1), (1, -1), (-1, -1), (-1, 1), (3, 4), (tiny, 2 * tiny), (tiny, huge), (huge, tiny),
                (-huge, -tiny))),
    ]


def hyperbolic_regions(fmt: Format, wide: Number, overflow_low: Number, overflow_high: Number) -> list[Region]:
    return [
        Cases(400, (UniformIn(-wide, wide),)),
        Cases(250, (UniformIn(-1, 1),)),
        *tiny_regions(fmt),
        # DetMath switches formulas at 1/8 (Tanh), 1/4 (Sinh) and 22.
        Cases(50, (Around((Fraction(1, 4), Fraction(1, 8)), 32, signed=True),)),
        Cases(50, (Around((22,), 32, signed=True),)),
        Cases(150, (Binades(overflow_low, overflow_high, signed=True),)),
        Listed(((1,), (-1,), (Fraction(1, 2),), (22,), (overflow_low,), (overflow_high,))),
    ]


def sinh_cosh_regions(fmt: Format) -> list[Region]:
    if fmt is DOUBLE:
        return hyperbolic_regions(fmt, 700, 709, 711)
    return hyperbolic_regions(fmt, 88, 88, 90)


def tanh_regions(fmt: Format) -> list[Region]:
    wide = per_format(fmt, 40, 20)
    return [
        Cases(400, (UniformIn(-wide, wide),)),
        Cases(250, (UniformIn(-1, 1),)),
        *tiny_regions(fmt),
        Cases(50, (Around((Fraction(1, 8),), 32, signed=True),)),
        Cases(200, (Binades(18, 26, signed=True),)),
        Listed(((1,), (-1,), (Fraction(1, 8),), (22,), (-22,), (wide,))),
    ]


def exponential_regions(fmt: Format) -> list[Region]:
    edges: tuple[tuple[Number, ...], ...]
    if fmt is DOUBLE:
        wide = (-700, 700)
        overflow = (708, Fraction("709.79"))
        beyond = (Fraction("709.78"), 710)
        subnormal = (Fraction("-745.2"), Fraction("-708.4"))
        zero = (-746, -745)
        edges = ((1,), (-1,), (Fraction("709.78"),), (Fraction("709.79"),), (-745,), (-746,), (-740,))
    else:
        wide = (-87, 88)
        overflow = (87, Fraction("88.72"))
        beyond = (Fraction("88.7"), 89)
        subnormal = (Fraction("-103.97"), Fraction("-87.34"))
        zero = (-104, Fraction("-103.2"))
        edges = ((1,), (-1,), (Fraction("88.7"),), (Fraction("88.8"),), (-100,), (-104,))
    return [
        Cases(400, (UniformIn(*wide),)),
        Cases(200, (UniformIn(-1, 1),)),
        *tiny_regions(fmt),
        Cases(100, (UniformIn(*overflow),)),
        Cases(30, (UniformIn(*beyond),)),
        Cases(200, (UniformIn(*subnormal),)),
        Cases(50, (UniformIn(*zero),)),
        Listed(edges),
    ]


def logarithm_regions(fmt: Format, powers_of_ten: bool) -> list[Region]:
    decades = int(per_format(fmt, 300, 37))
    regions: list[Region] = [
        Cases(400, (Binades(Fraction(1, 10**decades), 10**decades),)),
        Cases(200, (AroundOne(power_of_two(-fmt.precision), power_of_two(-6)),)),
        Cases(100, (Binades(fmt.min_subnormal, fmt.min_normal),)),
        Cases(100, (Binades(fmt.min_subnormal, fmt.max_finite),)),
        # DetMath's mantissa range is [0.75, 1.5).
        Cases(50, (Around((Fraction(3, 4), Fraction(3, 2)), 32),)),
        Listed(((1,), (2,), (10,), (Fraction(1, 2),), (fmt.max_finite,), (fmt.min_subnormal,), (fmt.min_normal,),
                (Fraction(2718281828459045, 10**15),))),
    ]
    if powers_of_ten:
        exact = int(per_format(fmt, 22, 10))
        regions.append(Listed(tuple((10**exponent,) for exponent in range(exact + 1))))
        regions.append(Cases(100, (Around(tuple(Fraction(10) ** exponent for exponent in range(-30, 31)), 4),)))
    return regions


def power_regions(fmt: Format) -> list[Region]:
    edges: tuple[tuple[Number, ...], ...]
    if fmt is DOUBLE:
        overflow: tuple[Number, Number] = (700, 710)
        subnormal: tuple[Number, Number] = (Fraction("-745.2"), -700)
        root_limit = 1 << 26
        edges = (
            (2, Fraction(1, 2)), (4, Fraction(1, 2)), (Fraction(1, 4), Fraction(-3, 2)), (10, 308), (10, 309),
            (10, -320), (10, 22), (2, -1074), (2, 1023), (2, -1075), (2, Fraction(-2149, 2)), (-2, 3), (-3, 3),
            (Fraction(-1, 2), 3), (1 + power_of_two(-52), power_of_two(52)), (7, 1), (Fraction(1, 10), 1),
        )
    else:
        overflow = (80, 90)
        subnormal = (-104, Fraction("-87.3"))
        root_limit = 1 << 12
        edges = (
            (2, Fraction(1, 2)), (4, Fraction(1, 2)), (Fraction(1, 4), Fraction(-3, 2)), (10, 38), (10, 39),
            (10, -40), (10, 10), (2, -149), (2, 127), (2, -150), (2, Fraction(-299, 2)), (-2, 3), (-3, 3),
            (Fraction(-1, 2), 3), (1 + power_of_two(-23), power_of_two(23)), (7, 1), (Fraction(1, 10), 1),
        )
    above_one = UniformIn(Fraction(3, 2), 1000)
    below_one = UniformIn(Fraction(1, 1000), Fraction(7, 10))
    return [
        Cases(400, (UniformIn(Fraction(1, 1000), 1000), UniformIn(-8, 8))),
        Cases(150, (UniformIn(Fraction(1, 1000), 1000), Integers(-40, 40))),
        Cases(100, (UniformIn(-1000, Fraction(-1, 1000)), Integers(-40, 40))),
        Cases(100, (AroundOne(power_of_two(1 - fmt.precision), power_of_two(-20)),
                    Binades(1, power_of_two(62), signed=True))),
        PowerTargets(75, above_one, *overflow),
        PowerTargets(75, below_one, *overflow),
        PowerTargets(75, above_one, *subnormal),
        PowerTargets(75, below_one, *subnormal),
        Cases(50, (Binades(fmt.min_subnormal, fmt.max_finite), UniformIn(-2, 2))),
        Cases(50, (PerfectSquares(root_limit), HalfIntegers(-20, 19))),
        Listed(edges),
    ]


@dataclasses.dataclass(frozen=True)
class FunctionSpec:
    """A DetMath function: the Evaluator method that computes it, its arity and the input regions of its tables."""

    method: str
    arity: int
    regions: Callable[[Format], list[Region]]


def logarithm_only(fmt: Format) -> list[Region]:
    return logarithm_regions(fmt, powers_of_ten=False)


def logarithm10(fmt: Format) -> list[Region]:
    return logarithm_regions(fmt, powers_of_ten=True)


DBL_MAX = DOUBLE.max_finite
FLT_MAX = FLOAT.max_finite
FUNCTIONS: dict[str, FunctionSpec] = {
    "Sin": FunctionSpec("sin", 1, trigonometric_regions),
    "Cos": FunctionSpec("cos", 1, trigonometric_regions),
    "Tan": FunctionSpec("tan", 1, trigonometric_regions),
    "ASin": FunctionSpec("asin", 1, inverse_sine_regions),
    "ACos": FunctionSpec("acos", 1, inverse_sine_regions),
    "ATan": FunctionSpec("atan", 1, arc_tangent_regions),
    "ATan2": FunctionSpec("atan2", 2, arc_tangent2_regions),
    "Sinh": FunctionSpec("sinh", 1, sinh_cosh_regions),
    "Cosh": FunctionSpec("cosh", 1, sinh_cosh_regions),
    "Tanh": FunctionSpec("tanh", 1, tanh_regions),
    "Exp": FunctionSpec("exp", 1, exponential_regions),
    "Log": FunctionSpec("log", 1, logarithm_only),
    "Log10": FunctionSpec("log10", 1, logarithm10),
    "Pow": FunctionSpec("pow", 2, power_regions),
}


def in_domain(function: str, fmt: Format, inputs: Inputs) -> bool:
    """Finite nonzero arguments inside the function's real domain; zeros, infinities, NaNs and the other special
    values are covered by the Annex F tests."""
    values = [fmt.to_fraction(bits) for bits in inputs]
    if any(value == 0 for value in values):
        return False
    if function in ("Log", "Log10"):
        return values[0] > 0
    if function in ("ASin", "ACos"):
        return abs(values[0]) <= 1
    if function == "Pow":
        return values[0] > 0 or values[1].denominator == 1
    return True


# --------------------------------------------------------------------------------------------------------------------
# Tables and the generated header
# --------------------------------------------------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Table:
    function: str
    fmt: Format
    descriptions: tuple[str, ...]
    rows: tuple[Row, ...]

    @property
    def name(self) -> str:
        return f"DetMath{self.function}{self.fmt.title}"


def table_inputs(function: str, fmt: Format) -> tuple[tuple[str, ...], list[Inputs]]:
    """The region descriptions and the inputs of one table: every region's inputs in order, without duplicates or
    out-of-domain arguments."""
    spec = FUNCTIONS[function]
    name = f"DetMath{function}{fmt.title}"
    regions = spec.regions(fmt)
    seen: set[Inputs] = set()
    inputs: list[Inputs] = []
    for index, region in enumerate(regions):
        for row in region.generate(SplitMix64(region_seed(name, index)), fmt):
            if len(row) != spec.arity:
                raise ValueError(f"{name}: region {index} produced {len(row)} arguments instead of {spec.arity}")
            if row not in seen and in_domain(function, fmt, row):
                seen.add(row)
                inputs.append(row)
    return tuple(region.describe(fmt) for region in regions), inputs


def build_table(function: str, fmt: Format, calculator: ReferenceCalculator) -> Table:
    descriptions, inputs = table_inputs(function, fmt)
    rows = tuple((row, calculator.reference(function, fmt, row)) for row in inputs)
    return Table(function, fmt, descriptions, rows)


HEADER_PREAMBLE = """#pragma once

// Correctly rounded references for the test "DetMath: within 1 ULP of correctly rounded references over seeded tables"
// (Tests/Source/Engine/Core/DetMathTests.cpp, Docs/Decisions/0007-detmath-reference-oracle.md).
//
// Generated by Scripts/GenerateDetMathReference.py; do not edit. Regenerate with
// `python Scripts/GenerateDetMathReference.py`: the output is byte-for-byte reproducible, and `--check` compares this
// file with a fresh run.
//
// Every entry holds IEEE-754 bit patterns: the arguments and the exact result rounded to nearest, ties to even, in the
// same format (a float result is rounded from the exact value, never through double). The arguments are finite and
// nonzero, drawn with SplitMix64 from seed {seed}. Each table lists the regions of its arguments, in order and
// without duplicates: "+-[a, b] by binade" is uniform over the format values in [a, b], either sign, and "max" is the
// largest finite value.

#include <array>
#include <cstdint>

namespace Engine {{

	namespace Test {{

		// One argument and the correctly rounded result.
		template<typename Bits>
		struct DetMathUnaryReference
		{{
			Bits X = 0;
			Bits Expected = 0;
		}};

		// Two arguments in DetMath's parameter order (ATan2(y, x), Pow(base, exponent)) and the correctly rounded result.
		template<typename Bits>
		struct DetMathBinaryReference
		{{
			Bits First = 0;
			Bits Second = 0;
			Bits Expected = 0;
		}};

		// clang-format off
"""

HEADER_END = """		// clang-format on

	}

}
"""

# Columns of a comment line in the tables: 120 minus two tabs (8 columns) and "// ".
COMMENT_WIDTH = 120 - 8 - 3


def render_header(tables: Sequence[Table]) -> str:
    lines = [HEADER_PREAMBLE.format(seed=f"{SEED:#x}").rstrip("\n")]
    for table in tables:
        arity = FUNCTIONS[table.function].arity
        structure = "DetMathUnaryReference" if arity == 1 else "DetMathBinaryReference"
        summary = (f"{table.function}, {table.fmt.name}, {len(table.rows)} cases: "
                   f"{'; '.join(table.descriptions)}.")
        lines.append("")
        for line in textwrap.wrap(summary, width=COMMENT_WIDTH, break_long_words=False, break_on_hyphens=False):
            lines.append(f"\t\t// {line}")
        lines.append(f"\t\tinline constexpr std::array<{structure}<{table.fmt.bits_type}>, {len(table.rows)}> "
                     f"{table.name} = {{ {{")
        for inputs, expected in table.rows:
            lines.append(f"\t\t\t{{ {', '.join(table.fmt.hex(bits) for bits in (*inputs, expected))} }},")
        lines.append("\t\t} };")
    lines.append("")
    return "\n".join(lines) + "\n" + HEADER_END


# --------------------------------------------------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------------------------------------------------


def double_bits(value: float) -> int:
    return int.from_bytes(struct.pack("<d", value), "little")


def float_bits(value: float) -> int:
    """The host's own double-to-float conversion (a C cast), for comparison only."""
    return int.from_bytes(struct.pack("<f", value), "little")


def rounding_failures() -> list[str]:
    failures: list[str] = []
    rng = SplitMix64(SEED ^ 0x5E1F7E57)

    def expect(fmt: Format, value: Fraction, expected: int, what: str) -> None:
        actual = fmt.round(value)
        if actual != expected:
            failures.append(f"rounding {what} to {fmt.name}: got {fmt.hex(actual)}, expected {fmt.hex(expected)}")

    for fmt in FORMATS:
        for _ in range(SELF_TEST_SAMPLES):
            bits = rng.below(fmt.infinity_bits) | (fmt.sign_bit if rng.coin() else 0)
            expect(fmt, fmt.to_fraction(bits), bits, f"the exact value of {fmt.hex(bits)}")
            positive = rng.below(fmt.max_finite_bits)
            low, high = fmt.to_fraction(positive), fmt.to_fraction(positive + 1)
            middle = (low + high) / 2
            nudge = (high - low) / (1 << 40)
            even = positive if positive % 2 == 0 else positive + 1
            expect(fmt, middle, even, f"the midpoint above {fmt.hex(positive)}")
            expect(fmt, middle - nudge, positive, f"just below the midpoint above {fmt.hex(positive)}")
            expect(fmt, middle + nudge, positive + 1, f"just above the midpoint above {fmt.hex(positive)}")
        tiny, huge = fmt.min_subnormal, fmt.max_finite
        half_ulp_of_max = power_of_two(fmt.max_exponent - fmt.precision)
        expect(fmt, tiny / 2, 0, "half the smallest subnormal (ties to +0)")
        expect(fmt, -tiny / 2, fmt.sign_bit, "minus half the smallest subnormal (ties to -0)")
        expect(fmt, tiny / 2 + tiny / (1 << 30), 1, "just above half the smallest subnormal")
        expect(fmt, 3 * tiny / 2, 2, "1.5 times the smallest subnormal (ties to even)")
        expect(fmt, fmt.min_normal - tiny / 2, fmt.round(fmt.min_normal), "the tie below the smallest normal")
        expect(fmt, huge + half_ulp_of_max, fmt.infinity_bits, "max plus half an ULP (overflows)")
        expect(fmt, huge + half_ulp_of_max - tiny, fmt.max_finite_bits, "just below max plus half an ULP")
        expect(fmt, 1 + power_of_two(-fmt.precision), fmt.round(Fraction(1)), "1 plus half an ULP (ties to even)")
    for _ in range(SELF_TEST_SAMPLES):
        # The host's correctly rounded int / int division and its double-to-float cast, as independent references.
        numerator = rng.next() >> rng.below(64)
        denominator = (rng.next() >> rng.below(64)) | 1
        value = Fraction(numerator, denominator) * power_of_two(rng.below(2100) - 1150)
        expect(DOUBLE, value, double_bits(value.numerator / value.denominator), f"{value} (host division)")
        source = DOUBLE.to_fraction(Binades(power_of_two(-160), FLT_MAX).sample(rng, DOUBLE))
        expect(FLOAT, source, float_bits(float(source)), f"{float(source)!r} (host cast)")
    return failures


# Correctly rounded values that DetMathTests.cpp commits, computed independently of this script (the huge trigonometric
# arguments with 60-digit arithmetic and a 780-digit pi), then exact cases next to rounding boundaries:
# (function, format, arguments, expected), arguments and results as hex floats.
KNOWN_VALUES: tuple[tuple[str, Format, tuple[str, ...], str], ...] = (
    *((function, DOUBLE, (argument,), expected)
      for argument, results in (
          ("0x1p+19", ("0x1.57481ec90fde3p-3", "0x1.f8c1986ca67fap-1", "0x1.5c354a31a846ep-3")),
          ("0x1.0000000000001p+19", ("0x1.57481ecd01616p-3", "0x1.f8c1986c7b96ap-1", "0x1.5c354a35c5e0fp-3")),
          ((1e6).hex(), ("-0x1.6664b2568d867p-2", "0x1.df9df9906d32cp-1", "-0x1.7e9768ab734cp-2")),
          ((3e9).hex(), ("0x1.f958b458cc91bp-1", "-0x1.4917f746fa4fp-3", "-0x1.891b289d24f04p+2")),
          ((1e22).hex(), ("-0x1.b453ab76bf397p-1", "0x1.0be2cef01c8f4p-1", "-0x1.a0f79c1b6b257p+0")),
          ((1e100).hex(), ("-0x1.85c5e5b929359p-2", "0x1.d9757496841f5p-1", "-0x1.a5807d6f76f7dp-2")),
          ("0x1.6ac5b262ca1ffp+849", ("0x1p+0", "-0x1.14ae72e6ba22fp-61", "-0x1.d9ba9a7975636p+60")),
          ("0x1.fffffffffffffp+1023", ("0x1.452fc98b34e97p-8", "-0x1.fffe62ecfab75p-1", "-0x1.4530cfe729484p-8")),
      )
      for function, expected in zip(("Sin", "Cos", "Tan"), results)),
    *((function, FLOAT, (argument,), expected)
      for argument, results in (
          ((1e10).hex(), ("-0x1.f334c8p-2", "0x1.bf098ap-1", "-0x1.1dep-1")),
          ((1e30).hex(), ("-0x1.95136p-1", "-0x1.392444p-1", "0x1.4b2876p+0")),
          ("0x1.fffffep+127", ("-0x1.0b3366p-1", "0x1.b4bf2cp-1", "-0x1.393d94p-1")),
      )
      for function, expected in zip(("Sin", "Cos", "Tan"), results)),
    ("Sin", DOUBLE, ("0x1.921fb54442d18p+1",), "0x1.1a62633145c07p-53"),
    ("Cos", DOUBLE, ("0x1.921fb54442d18p+1",), "-0x1p+0"),
    ("Sin", DOUBLE, ("0x1p-1",), (0.479425538604203).hex()),
    ("Cos", DOUBLE, ("0x1p-1",), (0.8775825618903728).hex()),
    ("Tan", DOUBLE, ("0x1.921fb54442d18p-1",), (0.9999999999999999).hex()),
    ("ATan", DOUBLE, ("0x1p+0",), (0.7853981633974483).hex()),
    ("ASin", DOUBLE, ("0x1p+0",), (1.5707963267948966).hex()),
    ("ACos", DOUBLE, ("-0x1p+0",), "0x1.921fb54442d18p+1"),
    ("ACos", DOUBLE, ("0x0p+0",), (1.5707963267948966).hex()),
    ("Exp", DOUBLE, ("0x1p+0",), (2.718281828459045).hex()),
    ("Exp", DOUBLE, ("-0x1p+0",), (0.36787944117144233).hex()),
    ("Exp", DOUBLE, ((-745.0).hex(),), "0x0.0000000000001p-1022"),
    ("Log", DOUBLE, ("0x1p+1",), (0.6931471805599453).hex()),
    ("Log", DOUBLE, ("0x1.4p+3",), (2.302585092994046).hex()),
    ("Log10", DOUBLE, ("0x1p+1",), (0.3010299956639812).hex()),
    ("Sinh", DOUBLE, ("0x1p+0",), (1.1752011936438014).hex()),
    ("Cosh", DOUBLE, ("0x1p+0",), (1.5430806348152437).hex()),
    ("Tanh", DOUBLE, ("0x1p+0",), (0.7615941559557649).hex()),
    ("ATan2", DOUBLE, ("0x1p+0", "-0x1p+0"), (2.356194490192345).hex()),
    ("Pow", DOUBLE, ("0x1p+1", "0x1p-1"), (1.4142135623730951).hex()),
    ("Pow", DOUBLE, ("0x1p+1", (-1075.0).hex()), "0x0p+0"),
    ("Pow", DOUBLE, ("0x1p+1", (-1074.0).hex()), "0x0.0000000000001p-1022"),
    ("Pow", DOUBLE, ("0x1p+1", (-1074.5).hex()), "0x0.0000000000001p-1022"),
    ("Pow", DOUBLE, ("0x1p+2", "0x1p-1"), "0x1p+1"),
    ("Pow", DOUBLE, ("0x1p-2", "-0x1.8p+0"), "0x1p+3"),
    ("Pow", DOUBLE, ("-0x1p+1", "0x1.8p+1"), "-0x1p+3"),
    ("Pow", FLOAT, ("0x1p+1", (-150.0).hex()), "0x0p+0"),
    ("Pow", FLOAT, ("0x1p+1", (-149.0).hex()), "0x1p-149"),
    ("Log", DOUBLE, ("0x1p+0",), "0x0p+0"),
    ("Log10", DOUBLE, ("0x1p+0",), "0x0p+0"),
    ("Log10", DOUBLE, ((1000.0).hex(),), (3.0).hex()),
    ("Log10", DOUBLE, ((1e22).hex(),), (22.0).hex()),
    ("ACos", DOUBLE, ("0x1p+0",), "0x0p+0"),
)


def hex_to_bits(text: str, fmt: Format) -> int:
    return fmt.round(Fraction(float.fromhex(text)))


def known_value_failures(calculator: ReferenceCalculator) -> list[str]:
    failures: list[str] = []
    for function, fmt, arguments, expected_text in KNOWN_VALUES:
        inputs = tuple(hex_to_bits(argument, fmt) for argument in arguments)
        expected = hex_to_bits(expected_text, fmt)
        actual = calculator.reference(function, fmt, inputs)
        if actual != expected:
            failures.append(f"{function}({', '.join(arguments)}) in {fmt.name}: got {fmt.hex(actual)}, expected "
                            f"{fmt.hex(expected)} ({expected_text})")
    return failures


def self_test(calculator: ReferenceCalculator) -> list[str]:
    failures = rounding_failures()
    if not str(calculator.pi).startswith(PI_LEADING_DIGITS):
        failures.append(f"pi starts with {str(calculator.pi)[:len(PI_LEADING_DIGITS)]}")
    failures += known_value_failures(calculator)
    return failures


# --------------------------------------------------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------------------------------------------------


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate the correctly rounded DetMath reference cases "
                    f"({paths.display_path(OUTPUT_PATH)}).",
        epilog="Exit codes: 0 success, 1 self-test failure, undecided reference or --check difference, 2 usage error.",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="compare a fresh run with the committed file; write nothing")
    mode.add_argument("--self-test", action="store_true", help="run only the self-test")
    parser.add_argument("--output", type=Path, default=OUTPUT_PATH,
                        help=f"the header to write or check (default: {paths.display_path(OUTPUT_PATH)})")
    parser.add_argument("--json", action="store_true", help="print a machine-readable result on stdout")
    return parser.parse_args(argv)


def generate_tables(calculator: ReferenceCalculator, console: Console) -> tuple[list[Table], list[Step]]:
    tables: list[Table] = []
    steps: list[Step] = []
    for function in FUNCTIONS:
        for fmt in FORMATS:
            start = time.perf_counter()
            name = f"DetMath{function}{fmt.title}"
            try:
                table = build_table(function, fmt, calculator)
            except ReferenceUndecidedError as error:
                steps.append(Step(name, Status.FAILED, str(error), time.perf_counter() - start))
                console.result(steps[-1])
                return tables, steps
            tables.append(table)
            steps.append(Step(name, Status.PASSED, f"{len(table.rows)} cases", time.perf_counter() - start,
                              data={"cases": len(table.rows)}))
            console.result(steps[-1])
    return tables, steps


def main(argv: list[str] | None = None) -> int:
    configure_stdio()
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    console = Console(arguments.json)
    output: Path = arguments.output.resolve()
    console.heading("DetMath references")

    start = time.perf_counter()
    calculator = ReferenceCalculator()
    failures = self_test(calculator)
    for failure in failures:
        console.print(f"  {failure}")
    steps = [Step("self-test", Status.FAILED if failures else Status.PASSED,
                  f"{len(failures)} failure(s)" if failures else "rounding, pi and known values",
                  time.perf_counter() - start)]
    console.result(steps[-1])

    if not failures and not arguments.self_test:
        tables, table_steps = generate_tables(calculator, console)
        steps += table_steps
        if not any(step.failed for step in table_steps):
            content = render_header(tables).encode("utf-8")
            total = sum(len(table.rows) for table in tables)
            current = output.read_bytes() if output.is_file() else None
            if arguments.check:
                same = current == content
                steps.append(Step("check", Status.PASSED if same else Status.FAILED,
                                  f"{paths.display_path(output)} is {'current' if same else 'out of date'}"))
            elif current == content:
                steps.append(Step("write", Status.PASSED, f"{paths.display_path(output)} unchanged ({total} cases)"))
            else:
                output.write_bytes(content)
                steps.append(Step("write", Status.PASSED, f"wrote {paths.display_path(output)} ({total} cases)"))
            steps[-1].detail += f"; {calculator.escalations} case(s) needed more than {WORKING_DIGITS[0]} digits"
            steps[-1].data["escalations"] = calculator.escalations
            console.result(steps[-1])

    exit_code = overall_exit_code(steps)
    console.summary("Summary", [step for step in steps if not step.name.startswith("DetMath") or step.failed])
    if arguments.json:
        emit_json({"success": exit_code == EXIT_SUCCESS, "output": output.as_posix(),
                   "steps": [step.to_json() for step in steps]})
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
