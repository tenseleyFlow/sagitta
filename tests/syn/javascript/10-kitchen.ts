@sealed
export declare abstract class Box<T extends object> implements Readonly<T> {
    public readonly value: T;
    protected accessor count: number;
    private method<U>(arg: U): U { return arg satisfies U as const; }
}
interface Shape { area: number }
type Choice = string | number;
enum Kind { One, Two }
namespace Space { export const value = /x+/g; }
const nested = `a${`b${{c: /x/.test("x")}}`}`;
let div = value / 2;
if (value) /knownWrong/.test(value);
/* TODO */
.;
