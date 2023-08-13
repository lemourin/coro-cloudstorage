#include <iostream>

#include "coro/cloudstorage/util/evaluate_javascript.h"

using ::coro::cloudstorage::util::js::EvaluateJavascript;
using ::coro::cloudstorage::util::js::Function;

const std::string kYouTubeDescrambler = R"(
{
    var a = "LRqiJMJ3NFHTAqkzT";
    var b = a.split(""),
        c = [function() {
                for (var d = 64, e = []; ++d - e.length - 32;) switch (d) {
                    case 46:
                        d = 95;
                    default:
                        e.push(String.fromCharCode(d));
                    case 94:
                    case 95:
                    case 96:
                        break;
                    case 123:
                        d -= 76;
                    case 92:
                    case 93:
                        continue;
                    case 58:
                        d = 44;
                    case 91:
                }
                return e
            },
            function() {
                for (var d = 64, e = []; ++d - e.length - 32;) {
                    switch (d) {
                        case 91:
                            d = 44;
                            continue;
                        case 123:
                            d = 65;
                            break;
                        case 65:
                            d -= 18;
                            continue;
                        case 58:
                            d = 96;
                            continue;
                        case 46:
                            d = 95
                    }
                    e.push(String.fromCharCode(d))
                }
                return e
            },
            -1562215807, 151646303, -1720639601, -11687903, -1720639601, 17953897, -342507982, 132305508,
            function(d, e) {
                d = (d % e.length + e.length) % e.length;
                e.splice(-d).reverse().forEach(function(f) {
                    e.unshift(f)
                })
            },
            null, 1239297289, "function", -1748489203, -805547473,
            function(d, e, f, h, l, m) {
                return e(h, l, m)
            },
            function(d) {
                d.reverse()
            },
            -946196920, -376614275, 646460387,
            function(d) {
                for (var e = d.length; e;) d.push(d.splice(--e, 1)[0])
            },
            -754632437,
            function(d, e, f) {
                var h = f.length;
                e.forEach(function(l, m, n) {
                    this.push(n[m] = f[(f.indexOf(l) - f.indexOf(this[m]) + m + h--) % f.length])
                }, d.split(""))
            },
            1230848700, 1141559788, 512251546,
            function(d, e) {
                0 != d.length && (e = (e % d.length + d.length) % d.length, d.splice(0, 1, d.splice(e, 1, d[0])[0]))
            },
            2062630819, 1812348556, -1665687329, -1477197373, -985065856, -1688870093, b, b, -1114542430, 1478322686,
            function(d, e, f, h, l, m, n) {
                return d(l, m, n)
            },
            1491001466,
            function(d, e) {
                e = (e % d.length + d.length) % d.length;
                d.splice(e, 1)
            },
            -1375912564, -1054161009, 1407697955, -1604961026, 172914366, 494058292, "[\u2247;", -1157925612, 1992076422, -1477197373,
            function(d, e) {
                e.splice(e.length, 0, d)
            },
            function() {
                for (var d = 64, e = []; ++d - e.length - 32;) {
                    switch (d) {
                        case 58:
                            d -= 14;
                        case 91:
                        case 92:
                        case 93:
                            continue;
                        case 123:
                            d = 47;
                        case 94:
                        case 95:
                        case 96:
                            continue;
                        case 46:
                            d = 95
                    }
                    e.push(String.fromCharCode(d))
                }
                return e
            },
            function() {
                for (var d = 64, e = []; ++d - e.length - 32;) switch (d) {
                    case 58:
                        d = 96;
                        continue;
                    case 91:
                        d = 44;
                        break;
                    case 65:
                        d = 47;
                        continue;
                    case 46:
                        d = 153;
                    case 123:
                        d -= 58;
                    default:
                        e.push(String.fromCharCode(d))
                }
                return e
            },
            function(d, e, f, h, l, m, n, p, q, t, u) {
                return d(f, h, l, m, n, p, q, t, u)
            },
            -859337116, null, -547750551, -473054614,
            function(d, e, f, h, l, m, n, p, q) {
                return f(h, l, m, n, p, q)
            },
            448193866, -858979502, 759472042, 8366080, -312533407, -1424817794, -1331307446,
            function(d, e) {
                for (e = (e % d.length + d.length) % d.length; e--;) d.unshift(d.pop())
            },
            function(d, e, f, h, l) {
                return e(f, h, l)
            },
            834676439, -1852754321, null,
            function(d, e) {
                if (0 != d.length) {
                    e = (e % d.length + d.length) % d.length;
                    var f = d[0];
                    d[0] = d[e];
                    d[e] = f
                }
            },
            646460387, -1659032399, -2146099728, -36593675, -1257340476, b
        ];
    c[11] = c;
    c[56] = c;
    c[71] = c;
    try {
        try {
            (4 != c[33] || ((0, c[16])((((0, c[27])(c[56], c[57]), ((0, c[72])(c[71], c[76]), c[23])(c[13], c[34], (0, c[53])()), c[40])(c[78], c[new Date("1969-12-31T22:00:43.000-02:00") / 1E3]), c[23])(c[13], c[78], (0, c[191 * Math.pow(8, 3) + -97739])()), c[23], (0, c[21])(c[34]), c[109 + Math.pow(3, 2) - 105], c[78], (0, c[53])()), 0)) && (0, c[59])((0, c[17])(c[35]), (0, c[67])(c[56], c[61]), c[21 + Math.pow(2, 1) + 25], (0, c[48])((0, c[47])(c[51], c[77]), c[150 % Math.pow(2,
                2) + 58], c[42]), c[70], c[42], c[33]) << ((0, c[60])(c[54]), c[25])(c[56], c[24]), (0, c[new Date("1970-01-01T08:00:56.000+08:00") / 1E3])(c[51], c[75]), (0, c[5])(c[new Date("1969-12-31T23:00:15.000-01:00") / 1E3], c[29], (0, c[55])())
        } catch (d) {
            2 < c[56 + Math.pow(2, 1) - -10] && (c[64] == new Date("1970-01-01T05:30:04.000+05:30") / 1E3 || ((0, c[5])(c[15], c[72], (0, c[28])()), ((0, c[39])((0, c[11])(c[51]), c[Math.pow(1, 2) - 37 - -96], c[31], c[61]), c[17])(c[11], c[71]), 0)) && ((((0, c[17])(c[55], c[65]), c[0])(c[69], c[12], (0, c[57])()), c[4])(c[207 %
                Math.pow(6, 2) + 6], c[68]), c[77])(c[33])
        }
        try {
            (c[new Date("1969-12-31T22:45:21.000-01:15") / 1E3] >= Math.pow(1, 1) % 474 + -6 || (((0, c[45])((0, c[72])((0, c[44])(c[11], c[47]), c[28], ((((0, c[77])(c[new Date("1970-01-01T09:45:55.000+09:45") / 1E3]), c[Math.pow(8, 2) + 115 + -135])(c[12], c[1]), c[4])(c[33], c[16]), c[4])(c[67], c[78]), c[16], c[33]), c[66], c[52], c[67]), c[18])(c[73], c[43]), NaN)) && (0, c[63])(c[47], ((0, c[77])((0, c[18])(c[22], c[0]), c[54], c[21], c[43], (0, c[8])()), c[54])(c[21], c[new Date("1969-12-31T18:45:07.000-05:15") /
                1E3], (0, c[61])()), c[54], (0, c[54])(c[21], c[44], (0, c[8])()), (0, c[77])((0, c[49])(c[44], c[42]), c[1], c[0], c[12]), (0, c[54])(c[21], c[43], (0, c[62])()), c[new Date("1969-12-31T13:30:21.000-10:30") / 1E3], c[44], (0, c[70 + Math.pow(8, 2) + -73])()), c[46] > -702 - -178 * Math.pow(4, 1) && (0, c[49])(c[43], c[45]), 5 >= c[70] && (9 == c[23] && ((0, c[77])((0, c[49])(c[44], c[21223 + -98 * Math.pow(6, 3)]), c[49], c[65], c[46]), 1) || (0, c[77])((0, c[76])(c[43], c[3]), c[36], c[7], c[50])), 1 !== c[3] && (-8 != c[16] ? (0, c[77])(((((0, c[77])((0, c[new Date("1970-01-01T07:45:18.000+07:45") /
                1E3])(c[57], c[19]), c[69], c[new Date("1969-12-31T13:45:32.000-10:15") / 1E3], c[74]), c[24])(c[75], c[47]), ((0, c[13])(c[75]), c[6])(c[59], c[7]), c[48])(c[65], c[74]), c[66])(c[49], c[22]), c[31], c[30], c[46]) : (0, c[27])(((((((0, c[79])(c[15], c[1]), c[4])(c[51], c[37], (0, c[38])()), c[79])(c[30], c[35]), (0, c[4])(c[51], c[74], (0, c[12])()), c[31])(c[49], c[25]), c[4])(c[51], c[74], (0, c[11])()), c[48])(c[8], c[73]), c[55], c[37])), 4 >= c[3] && ((0, c[27])((0, c[66])(c[15], c[67]), c[79], c[73], c[14]), (0, c[48])(c[3], c[74]), 1) || (0, c[27])((0, c[27])((0, c[59])(c[Math.pow(1,
                5) - -31710 + -31662]), c[4], c[51], c[37], (0, c[11])()), c[66], c[30], c[57]), -9 > c[16] && (0, c[66])(c[96 - 228 % Math.pow(3, 4)], c[45]), 8 !== c[42] && (9 <= c[1] || (((0, c[15 * Math.pow(8, 3) + -7653])((0, c[10])(c[69], c[15]), c[26], c[73], c[80]), c[27])((0, c[26])(c[49], c[68]), c[28], c[new Date("1970-01-01T07:31:13.000+07:30") / 1E3], c[2]), 0)) && ((0, c[21])(c[36]) <= (0, c[69])(c[36], c[3])) * (0, c[21])(c[11]) >= (0, c[47])(c[13], c[35], (0, c[54])()), (-8 >= c[79] || ((0, c[21])(c[11]), 0)) && (0, c[74])(c[11], c[26])
        } catch (d) {
            ((((0, c[70])((0, c[47])(c[13],
                c[36], (0, c[54])()), c[41], c[0], c[5]), c[41])(c[36], c[75]), c[17])(c[35]), c[69])(c[36], c[33]), (0, c[17])(c[36]), (0, c[70])((0, c[21])(c[35]), c[41], c[35], c[67])
        }
        try {
            -7 < c[48] && (4 === c[14] && ((0, c[47])(c[13], c[35], (0, c[8])()), 6) || (0, c[17])(c[398 % Math.pow(4, 2) + new Date("1970-01-01T04:45:22.000+04:45") / 1E3]))
        } catch (d) {
            (0, c[17])(c[36])
        }
    } catch (d) {
        return "enhanced_except_hpkB-eb-_w8_" + a
    }
    return b.join("")
};
)";

const std::string kTestCase = R"({
  var e = [];
  return (e.length-32);
})";

int main() {
  try {
    std::cerr << EvaluateJavascript(Function{.source = kYouTubeDescrambler}, {})
              << "\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
  }
}