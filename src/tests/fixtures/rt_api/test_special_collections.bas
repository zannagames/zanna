' test_special_collections.bas — BitSet, BloomFilter, UnionFind, Bytes, LruCache, MultiMap
DIM bs AS Zanna.Collections.BitSet
bs = Zanna.Collections.BitSet.New(64)
PRINT "bs empty: "; bs.IsEmpty
PRINT "bs len: "; bs.Length
bs.SetBit(3)
bs.SetBit(7)
bs.SetBit(15)
PRINT "bs count: "; bs.Count
PRINT "bs get 3: "; bs.GetBit(3)
PRINT "bs get 4: "; bs.GetBit(4)
bs.ToggleBit(3)
PRINT "bs get 3 after toggle: "; bs.GetBit(3)
bs.ClearBit(7)
PRINT "bs count after clear: "; bs.Count
PRINT "bs tostring: "; bs.ToString()
bs.Clear()
PRINT "bs count after clearall: "; bs.Count

DIM bf AS Zanna.Collections.BloomFilter
bf = Zanna.Collections.BloomFilter.New(1000, 0.01)
bf.Add("hello")
bf.Add("world")
PRINT "bf count: "; bf.Count
PRINT "bf might hello: "; bf.MightContain("hello")
PRINT "bf might xyz: "; bf.MightContain("xyz")

DIM uf AS Zanna.Collections.UnionFind
uf = Zanna.Collections.UnionFind.New(10)
PRINT "uf count: "; uf.Count
uf.Union(1, 2)
uf.Union(3, 4)
uf.Union(2, 3)
PRINT "uf connected 1,4: "; uf.IsConnected(1, 4)
PRINT "uf connected 1,5: "; uf.IsConnected(1, 5)
PRINT "uf setsize 1: "; uf.ComponentSize(1)

DIM by AS Zanna.Collections.Bytes
by = Zanna.Collections.Bytes.New(8)
PRINT "by len: "; by.Length
by.Set(0, 65)
by.Set(1, 66)
PRINT "by get 0: "; by.Get(0)
PRINT "by get 1: "; by.Get(1)
by.Fill(0)
PRINT "by get 0 after fill: "; by.Get(0)
PRINT "by tohex: "; by.ToHex()

DIM lru AS Zanna.Collections.LruCache
lru = Zanna.Collections.LruCache.New(3)
PRINT "lru empty: "; lru.IsEmpty
PRINT "lru cap: "; lru.Capacity
lru.Set("a", "1")
lru.Set("b", "2")
lru.Set("c", "3")
PRINT "lru count: "; lru.Count
PRINT "lru has a: "; lru.Has("a")
lru.Set("d", "4")
PRINT "lru count after evict: "; lru.Count
lru.Remove("d")
PRINT "lru count after remove: "; lru.Count

DIM mm AS Zanna.Collections.MultiMap
mm = Zanna.Collections.MultiMap.New()
PRINT "mm empty: "; mm.IsEmpty
mm.Add("color", "red")
mm.Add("color", "blue")
mm.Add("size", "large")
PRINT "mm count: "; mm.Count
PRINT "mm keycount: "; mm.KeyCount
PRINT "mm countfor color: "; mm.CountFor("color")
PRINT "mm has color: "; mm.Has("color")
mm.RemoveAll("color")
PRINT "mm count after removeall: "; mm.Count

PRINT "done"
END
