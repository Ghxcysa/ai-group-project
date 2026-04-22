import { Badge } from "@/components/ui/badge";
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from "@/components/ui/table";

interface GroupTableProps {
  groups: number[][];
  /** If provided, a delete button is shown at the end of each row */
  onDelete?: (index: number) => void;
  /** Optional set of sample numbers to highlight */
  highlightSamples?: Set<number>;
}

export default function GroupTable({
  groups,
  onDelete,
  highlightSamples,
}: GroupTableProps) {
  if (groups.length === 0) {
    return (
      <p className="py-8 text-center text-sm text-muted-foreground">
        No data
      </p>
    );
  }

  return (
    <Table>
      <TableHeader>
        <TableRow>
          <TableHead className="w-12 text-center">#</TableHead>
          <TableHead>k-Group Numbers</TableHead>
          {onDelete && <TableHead className="w-16 text-center">Action</TableHead>}
        </TableRow>
      </TableHeader>
      <TableBody>
        {groups.map((group, idx) => (
          <TableRow key={idx}>
            <TableCell className="text-center text-muted-foreground text-sm">
              {idx + 1}
            </TableCell>
            <TableCell>
              <div className="flex flex-wrap gap-1">
                {group.map((num) => (
                  <Badge
                    key={num}
                    variant={
                      highlightSamples && highlightSamples.has(num)
                        ? "default"
                        : "outline"
                    }
                    className="text-xs font-mono"
                  >
                    {num}
                  </Badge>
                ))}
              </div>
            </TableCell>
            {onDelete && (
              <TableCell className="text-center">
                <button
                  onClick={() => onDelete(idx)}
                  className="text-xs text-destructive hover:underline"
                >
                  Delete
                </button>
              </TableCell>
            )}
          </TableRow>
        ))}
      </TableBody>
    </Table>
  );
}
