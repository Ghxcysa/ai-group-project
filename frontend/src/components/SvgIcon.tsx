import { SVGProps } from "react";

type SvgIconProps = SVGProps<SVGSVGElement> & {
  icon: React.FC<SVGProps<SVGSVGElement>>;
  size?: number | string;
  className?: string;
};

export default function SvgIcon({
  icon: Icon,
  size = 20,
  className = "",
  ...props
}: SvgIconProps) {
  return (
    <Icon
      width={size}
      height={size}
      className={className}
      {...props}
    />
  );
}
