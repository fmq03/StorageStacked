module bridge_fifo #(
    parameter int WIDTH = 8,
    parameter int DEPTH = 4
) (
    input  logic                     clk,
    input  logic                     rst_n,
    input  logic                     push_valid,
    output logic                     push_ready,
    input  logic [WIDTH-1:0]         push_data,
    output logic                     pop_valid,
    input  logic                     pop_ready,
    output logic [WIDTH-1:0]         pop_data,
    output logic [$clog2(DEPTH+1)-1:0] count
);
    localparam int PTR_W = (DEPTH <= 2) ? 1 : $clog2(DEPTH);

    logic [WIDTH-1:0] storage [0:DEPTH-1];
    logic [PTR_W-1:0] head;
    logic [PTR_W-1:0] tail;
    logic push_fire;
    logic pop_fire;

    function automatic logic [PTR_W-1:0] ptr_next(
        input logic [PTR_W-1:0] value
    );
        if (int'(value) == DEPTH - 1) ptr_next = '0;
        else ptr_next = value + 1'b1;
    endfunction

    initial begin
        if (WIDTH < 1 || DEPTH < 1)
            $error("bridge_fifo WIDTH and DEPTH must be positive");
    end

    assign push_ready = count < $clog2(DEPTH+1)'(DEPTH);
    assign pop_valid = count != 0;
    assign pop_data = storage[head];
    assign push_fire = push_valid && push_ready;
    assign pop_fire = pop_valid && pop_ready;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            head <= '0;
            tail <= '0;
            count <= '0;
            for (int i = 0; i < DEPTH; i = i + 1)
                storage[i] <= '0;
        end else begin
            if (push_fire) begin
                storage[tail] <= push_data;
                tail <= ptr_next(tail);
            end
            if (pop_fire)
                head <= ptr_next(head);
            case ({push_fire, pop_fire})
                2'b10: count <= count + 1'b1;
                2'b01: count <= count - 1'b1;
                default: count <= count;
            endcase
        end
    end
endmodule
