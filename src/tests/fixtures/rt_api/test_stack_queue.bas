' test_stack_queue.bas — Stack, Queue, Ring, Heap
DIM st AS Zanna.Collections.Stack
st = Zanna.Collections.Stack.New()
PRINT "stack empty: "; st.IsEmpty
st.Push("a")
st.Push("b")
st.Push("c")
PRINT "stack len: "; st.Count
st.Pop()
st.Pop()
PRINT "stack len after 2 pops: "; st.Count
st.Clear()
PRINT "stack empty after clear: "; st.IsEmpty

DIM q AS Zanna.Collections.Queue
q = Zanna.Collections.Queue.New()
PRINT "queue empty: "; q.IsEmpty
q.Push("x")
q.Push("y")
q.Push("z")
PRINT "queue len: "; q.Count
q.Pop()
q.Pop()
PRINT "queue len after 2 pops: "; q.Count
q.Clear()
PRINT "queue empty after clear: "; q.IsEmpty

DIM r AS Zanna.Collections.Ring
r = Zanna.Collections.Ring.NewDefault()
PRINT "ring empty: "; r.IsEmpty
r.Push("1")
r.Push("2")
r.Push("3")
PRINT "ring len: "; r.Count
r.Pop()
PRINT "ring len after pop: "; r.Count
r.Clear()
PRINT "ring empty after clear: "; r.IsEmpty

DIM h AS Zanna.Collections.Heap
h = Zanna.Collections.Heap.New()
PRINT "heap empty: "; h.IsEmpty
h.Push(3, "three")
h.Push(1, "one")
h.Push(2, "two")
PRINT "heap len: "; h.Count
h.Pop()
h.Pop()
PRINT "heap len after 2 pops: "; h.Count
h.Clear()
PRINT "heap empty after clear: "; h.IsEmpty

PRINT "done"
END
