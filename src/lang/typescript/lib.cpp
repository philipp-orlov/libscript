// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The types of everything the engine installs on the global object.
//
// These are written in TypeScript and parsed by the same parser that reads user
// code, so the description of the runtime cannot drift into a shape the parser
// would not accept. The cost is one parse of a few hundred lines per check.

#include "script/internal/dyn/checker.hpp"

namespace script {
namespace internal {
namespace dyn {

const char* globalDeclarations() noexcept
{
    return R"TS(
interface Object {
    toString(): string;
    toLocaleString(): string;
    valueOf(): any;
    hasOwnProperty(key: string): boolean;
    isPrototypeOf(other: any): boolean;
    propertyIsEnumerable(key: string): boolean;
}

interface Function {
    readonly name: string;
    readonly length: number;
    call(thisArg: any, ...args: any[]): any;
    apply(thisArg: any, args?: any[]): any;
    bind(thisArg: any, ...args: any[]): any;
    toString(): string;
}

interface IArguments {
    readonly length: number;
    [index: number]: any;
}

interface Iterator<T> {
    next(): { value: T; done: boolean };
}

/// Anything a `for...of` loop or a spread can walk.
interface Iterable<T> {
    [Symbol.iterator](): Iterator<T>;
}

interface Array<T> {
    length: number;
    [Symbol.iterator](): Iterator<T>;
    [index: number]: T;
    at(index: number): T | undefined;
    concat(...items: (T | T[])[]): T[];
    entries(): Iterator<[number, T]>;
    every(fn: (value: T, index: number, array: T[]) => boolean, thisArg?: any): boolean;
    fill(value: T, start?: number, end?: number): T[];
    filter(fn: (value: T, index: number, array: T[]) => boolean, thisArg?: any): T[];
    find(fn: (value: T, index: number, array: T[]) => boolean, thisArg?: any): T | undefined;
    findIndex(fn: (value: T, index: number, array: T[]) => boolean, thisArg?: any): number;
    flat(depth?: number): any[];
    flatMap(fn: (value: T, index: number, array: T[]) => any, thisArg?: any): any[];
    forEach(fn: (value: T, index: number, array: T[]) => void, thisArg?: any): void;
    includes(value: T, from?: number): boolean;
    indexOf(value: T, from?: number): number;
    join(separator?: string): string;
    keys(): Iterator<number>;
    lastIndexOf(value: T, from?: number): number;
    map<U>(fn: (value: T, index: number, array: T[]) => U, thisArg?: any): U[];
    pop(): T | undefined;
    push(...items: T[]): number;
    reduce<U>(fn: (acc: U, value: T, index: number, array: T[]) => U, initial?: U): U;
    reduceRight<U>(fn: (acc: U, value: T, index: number, array: T[]) => U, initial?: U): U;
    reverse(): T[];
    shift(): T | undefined;
    slice(start?: number, end?: number): T[];
    some(fn: (value: T, index: number, array: T[]) => boolean, thisArg?: any): boolean;
    sort(compare?: (a: T, b: T) => number): T[];
    splice(start: number, count?: number, ...items: T[]): T[];
    toString(): string;
    unshift(...items: T[]): number;
    values(): Iterator<T>;
}

interface RegExpExecArray extends Array<string> {
    index: number;
    input: string;
    groups: any;
    indices: any;
}

interface RegExp {
    lastIndex: number;
    readonly source: string;
    readonly flags: string;
    readonly global: boolean;
    readonly ignoreCase: boolean;
    readonly multiline: boolean;
    readonly dotAll: boolean;
    readonly unicode: boolean;
    readonly unicodeSets: boolean;
    readonly sticky: boolean;
    readonly hasIndices: boolean;
    exec(text: string): RegExpExecArray | null;
    test(text: string): boolean;
    toString(): string;
}

interface String {
    readonly length: number;
    [Symbol.iterator](): Iterator<string>;
    at(index: number): string | undefined;
    charAt(index: number): string;
    charCodeAt(index: number): number;
    codePointAt(index: number): number | undefined;
    concat(...parts: string[]): string;
    endsWith(search: string, end?: number): boolean;
    includes(search: string, from?: number): boolean;
    indexOf(search: string, from?: number): number;
    lastIndexOf(search: string, from?: number): number;
    localeCompare(other: string): number;
    normalize(form?: string): string;
    padEnd(length: number, pad?: string): string;
    padStart(length: number, pad?: string): string;
    repeat(count: number): string;
    match(pattern: string | RegExp): RegExpExecArray | null;
    matchAll(pattern: string | RegExp): Iterator<RegExpExecArray>;
    replace(search: string | RegExp, replacement: string | ((match: string, ...rest: any[]) => string)): string;
    replaceAll(search: string | RegExp, replacement: string | ((match: string, ...rest: any[]) => string)): string;
    search(pattern: string | RegExp): number;
    slice(start?: number, end?: number): string;
    split(separator?: string | RegExp, limit?: number): string[];
    startsWith(search: string, from?: number): boolean;
    substr(start: number, length?: number): string;
    substring(start: number, end?: number): string;
    toLowerCase(): string;
    toUpperCase(): string;
    toString(): string;
    trim(): string;
    trimEnd(): string;
    trimStart(): string;
    valueOf(): string;
    [index: number]: string;
}

interface Number {
    toExponential(digits?: number): string;
    toFixed(digits?: number): string;
    toLocaleString(): string;
    toPrecision(digits?: number): string;
    toString(radix?: number): string;
    valueOf(): number;
}

interface Boolean {
    toString(): string;
    valueOf(): boolean;
}

interface Symbol {
    readonly description: string | undefined;
    toString(): string;
}

interface Error {
    name: string;
    message: string;
    stack: string;
    toString(): string;
}

interface Date {
    getTime(): number;
    getFullYear(): number;
    toISOString(): string;
    toString(): string;
    toJSON(): string;
    valueOf(): number;
}

interface Map<K, V> {
    readonly size: number;
    [Symbol.iterator](): Iterator<[K, V]>;
    clear(): void;
    delete(key: K): boolean;
    entries(): Iterator<[K, V]>;
    forEach(fn: (value: V, key: K, map: Map<K, V>) => void, thisArg?: any): void;
    get(key: K): V | undefined;
    has(key: K): boolean;
    keys(): Iterator<K>;
    set(key: K, value: V): Map<K, V>;
    values(): Iterator<V>;
}

interface Set<T> {
    readonly size: number;
    [Symbol.iterator](): Iterator<T>;
    add(value: T): Set<T>;
    clear(): void;
    delete(value: T): boolean;
    entries(): Iterator<[T, T]>;
    forEach(fn: (value: T, value2: T, set: Set<T>) => void, thisArg?: any): void;
    has(value: T): boolean;
    keys(): Iterator<T>;
    values(): Iterator<T>;
}

interface Promise<T> {
    then<U>(onFulfilled?: (value: T) => U, onRejected?: (reason: any) => U): Promise<U>;
    catch<U>(onRejected: (reason: any) => U): Promise<U>;
    finally(onFinally: () => void): Promise<T>;
}

declare var Object: {
    new (value?: any): any;
    (value?: any): any;
    assign(target: any, ...sources: any[]): any;
    create(proto: any, properties?: any): any;
    defineProperty(target: any, key: string, descriptor: any): any;
    entries(target: any): [string, any][];
    freeze<T>(target: T): T;
    fromEntries(pairs: any[]): any;
    getOwnPropertyDescriptor(target: any, key: string): any;
    getOwnPropertyNames(target: any): string[];
    getPrototypeOf(target: any): any;
    is(a: any, b: any): boolean;
    isFrozen(target: any): boolean;
    keys(target: any): string[];
    preventExtensions<T>(target: T): T;
    setPrototypeOf(target: any, proto: any): any;
    values(target: any): any[];
    readonly prototype: Object;
};

declare var Array: {
    new <T>(length?: number): T[];
    <T>(length?: number): T[];
    isArray(value: any): boolean;
    from<T>(source: any, fn?: (value: any, index: number) => T): T[];
    of<T>(...items: T[]): T[];
    readonly prototype: any;
};

declare var String: {
    new (value?: any): String;
    (value?: any): string;
    fromCharCode(...codes: number[]): string;
    fromCodePoint(...codes: number[]): string;
    readonly prototype: String;
};

declare var Number: {
    new (value?: any): Number;
    (value?: any): number;
    isFinite(value: any): boolean;
    isInteger(value: any): boolean;
    isNaN(value: any): boolean;
    isSafeInteger(value: any): boolean;
    parseFloat(text: string): number;
    parseInt(text: string, radix?: number): number;
    readonly EPSILON: number;
    readonly MAX_SAFE_INTEGER: number;
    readonly MIN_SAFE_INTEGER: number;
    readonly MAX_VALUE: number;
    readonly MIN_VALUE: number;
    readonly NaN: number;
    readonly NEGATIVE_INFINITY: number;
    readonly POSITIVE_INFINITY: number;
    readonly prototype: Number;
};

declare var Boolean: {
    new (value?: any): Boolean;
    (value?: any): boolean;
    readonly prototype: Boolean;
};

declare var Symbol: {
    (description?: string): symbol;
    for(key: string): symbol;
    readonly iterator: symbol;
    readonly asyncIterator: symbol;
    readonly hasInstance: symbol;
    readonly toPrimitive: symbol;
    readonly toStringTag: symbol;
};

declare var Function: { readonly prototype: Function };

declare var Date: {
    new (value?: any): Date;
    (): string;
    now(): number;
    parse(text: string): number;
    readonly prototype: Date;
};

declare var RegExp: {
    new (pattern: string | RegExp, flags?: string): RegExp;
    (pattern: string | RegExp, flags?: string): RegExp;
    readonly prototype: RegExp;
    escape(text: string): string;
};

declare var Map: {
    new <K, V>(entries?: [K, V][]): Map<K, V>;
    readonly prototype: any;
};

declare var Set: {
    new <T>(values?: T[]): Set<T>;
    readonly prototype: any;
};

declare var Promise: {
    new <T>(executor: (resolve: (value: T) => void, reject: (reason: any) => void) => void): Promise<T>;
    all(values: any[]): Promise<any[]>;
    race(values: any[]): Promise<any>;
    reject<T>(reason?: any): Promise<T>;
    resolve<T>(value?: T): Promise<T>;
    readonly prototype: any;
};

declare var Math: {
    readonly E: number;
    readonly LN2: number;
    readonly LN10: number;
    readonly LOG2E: number;
    readonly LOG10E: number;
    readonly PI: number;
    readonly SQRT1_2: number;
    readonly SQRT2: number;
    abs(x: number): number;
    acos(x: number): number;
    asin(x: number): number;
    atan(x: number): number;
    atan2(y: number, x: number): number;
    cbrt(x: number): number;
    ceil(x: number): number;
    cos(x: number): number;
    cosh(x: number): number;
    exp(x: number): number;
    expm1(x: number): number;
    floor(x: number): number;
    fround(x: number): number;
    hypot(...values: number[]): number;
    log(x: number): number;
    log1p(x: number): number;
    log2(x: number): number;
    log10(x: number): number;
    max(...values: number[]): number;
    min(...values: number[]): number;
    pow(base: number, exponent: number): number;
    random(): number;
    round(x: number): number;
    sign(x: number): number;
    sin(x: number): number;
    sinh(x: number): number;
    sqrt(x: number): number;
    tan(x: number): number;
    tanh(x: number): number;
    trunc(x: number): number;
};

declare var JSON: {
    parse(text: string, reviver?: (key: string, value: any) => any): any;
    stringify(value: any, replacer?: any, space?: any): string;
};

declare var console: {
    log(...args: any[]): void;
    info(...args: any[]): void;
    warn(...args: any[]): void;
    error(...args: any[]): void;
    debug(...args: any[]): void;
    trace(...args: any[]): void;
};

declare var Error: { new (message?: string): Error; (message?: string): Error };
declare var TypeError: { new (message?: string): Error; (message?: string): Error };
declare var RangeError: { new (message?: string): Error; (message?: string): Error };
declare var SyntaxError: { new (message?: string): Error; (message?: string): Error };
declare var ReferenceError: { new (message?: string): Error; (message?: string): Error };
declare var EvalError: { new (message?: string): Error; (message?: string): Error };
declare var URIError: { new (message?: string): Error; (message?: string): Error };
declare var InternalError: { new (message?: string): Error; (message?: string): Error };

/// An approximation: the checker has no mapped types, so the key type is not checked.
type Record<K, T> = { [key: string]: T };

declare var globalThis: any;
declare var undefined: undefined;
declare var NaN: number;
declare var Infinity: number;

declare function parseInt(text: string, radix?: number): number;
declare function parseFloat(text: string): number;
declare function isNaN(value: any): boolean;
declare function isFinite(value: any): boolean;
declare function structuredClone<T>(value: T): T;
)TS";
}

}  // namespace dyn
}  // namespace internal
}  // namespace script
